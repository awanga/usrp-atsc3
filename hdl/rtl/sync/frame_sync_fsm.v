// frame_sync_fsm.v — frame synchronizer state machine and chunk procedure
//
// Bit-exact port of FrameSync::process(), set_bootstrap_offset() and the
// four handlers in lib/sync/frame_sync.cc (ATSC3_FIXED_POINT=ON), with the
// preamble correlation delegated to frame_correlator.v through the
// corr_* interface. Score, threshold and confidence are unsigned Q1.15
// (the model's FrameScore); +1.0 is 32767.
//
// One process() call is one chunk: proc_start with proc_n samples already
// in the correlator's RAM (chunk sample i at RAM address i). The procedure
// then runs the current state's handler exactly as the C++ does and adds
// proc_n to the global sample counter; proc_done pulses once at the end.
// In the C++ a chunk is whatever the caller passes to process(); here it
// is the TLAST-delimited packet.
//
//   SEARCHING  correlate at offset 0, F/4, F/2, ... while offset+F <= n;
//              first score >= threshold -> ACQUIRING (hits=1)
//   ACQUIRING  best score over a window around the expected boundary
//              (strictly greater wins, earliest offset); >= threshold:
//              blend 9/10, hits++, hits >= acquire count -> LOCKED + event;
//              else misses++, >= loss count -> SEARCHING
//   LOCKED     same search, relaxed threshold (4/5 of it): blend 19/20,
//              event, frame_number++, next boundary = chunk start + best
//              offset + frame_samples; else misses++, >= loss -> HOLDOVER
//   HOLDOVER   holdover_counter++; single correlation at the expected
//              offset: >= threshold -> LOCKED + event; else counter >=
//              holdover_frames -> SEARCHING, else frame_number++,
//              boundary += frame_samples, confidence * 9/10
// ACQUIRING's window loop ends at offset + F <= expected + half_window, so
// it can only succeed when F fits in search_window; the model has this
// property and the RTL keeps it.
//
// Configuration (cfg_*) is sampled at proc_start and held for the chunk
// (corr_len is the held fft_size, for the correlator).
// frame_samples = subframes_per_frame * symbols_per_subframe * (fft_size +
// cp_length), truncated to 48 bits; sample_index / expected boundary wrap
// at 2^48 (status_words.vh).
//
// bs_apply (one cycle, only while idle) is set_bootstrap_offset(): the
// expected boundary and confidence are loaded (confidence saturated to
// 32767), and SEARCHING / HOLDOVER move to ACQUIRING with hits=1.
//
// proc_skip makes the chunk a no-op except for the sample counter (used
// when the chunk could not be stored or the configuration is unusable).
//
// ev_valid pulses for one cycle per FrameEvent (at most one per chunk),
// with ev_* the event fields; the event type is always FRAME_START, and
// subframe/symbol numbers are zero because the model never advances them.

module frame_sync_fsm (
    input  wire        clk,
    input  wire        rst,  // synchronous, active-high

    input  wire [15:0] cfg_fft_size,
    input  wire [15:0] cfg_cp_length,
    input  wire [15:0] cfg_symbols_per_subframe,
    input  wire [15:0] cfg_subframes_per_frame,
    input  wire signed [15:0] cfg_threshold_q15,
    input  wire [15:0] cfg_lock_acquire_count,
    input  wire [15:0] cfg_lock_loss_count,
    input  wire [15:0] cfg_holdover_frames,
    input  wire [15:0] cfg_search_window,

    input  wire        bs_apply,
    input  wire [47:0] bs_offset,
    input  wire [15:0] bs_conf,

    input  wire        proc_start,
    input  wire        proc_skip,
    input  wire [31:0] proc_n,
    output reg         proc_done,

    output reg         corr_start,
    output reg  [31:0] corr_offset,
    output wire [15:0] corr_len,
    input  wire        corr_done,
    input  wire [15:0] corr_ratio,

    output reg         ev_valid,
    output reg  [47:0] ev_sample_index,
    output reg  [31:0] ev_frame_number,
    output reg  [15:0] ev_confidence,

    output wire [1:0]  mon_state,
    output wire [31:0] mon_frame_number,
    output wire [47:0] mon_expected_boundary,
    output wire [15:0] mon_confidence,
    output wire [47:0] mon_samples_to_boundary,
    output wire [47:0] mon_sample_counter
);

    localparam [1:0] FS_SEARCHING = 2'd0,
                     FS_ACQUIRING = 2'd1,
                     FS_LOCKED    = 2'd2,
                     FS_HOLDOVER  = 2'd3;

    localparam [4:0] S_IDLE      = 5'd0,
                     S_CFG0      = 5'd1,
                     S_CFG1      = 5'd2,
                     S_DISPATCH  = 5'd3,
                     S_SRCH_CHK  = 5'd4,
                     S_SRCH_WAIT = 5'd5,
                     S_ACQ_CHK   = 5'd6,
                     S_ACQ_WAIT  = 5'd7,
                     S_ACQ_EVAL  = 5'd8,
                     S_LCK_CHK   = 5'd9,
                     S_LCK_WAIT  = 5'd10,
                     S_LCK_EVAL  = 5'd11,
                     S_HLD_WAIT  = 5'd12,
                     S_HLD_EVAL  = 5'd13,
                     S_HLD_TAIL  = 5'd14,
                     S_FINISH    = 5'd15;

    localparam [15:0] SCORE_ONE = 16'd32767;

    reg [4:0] state;

    // Persistent synchronizer state (the C++ members)
    reg [47:0] sample_counter;

    // Per-chunk latched configuration and derived values
    reg [15:0] fft_r;
    reg [15:0] cp_r;
    reg [15:0] sym_r;
    reg [15:0] sub_r;
    reg [15:0] thr_r;      // detection threshold, 0..32767
    reg [15:0] relax_r;    // thr * 4 / 5
    reg [15:0] acq_r;
    reg [15:0] loss_r;
    reg [15:0] hold_r;
    reg [15:0] half_r;     // search_window / 2
    reg [31:0] n_r;
    reg [47:0] frame_samples;
    reg [47:0] sym_samples;

    // Loop state
    reg [31:0] off;
    reg [31:0] end_r;
    reg [15:0] best;
    reg [31:0] best_off;

    // Synchronizer members
    reg [1:0]  fs_state;
    reg [31:0] frame_number;
    reg [47:0] expected;
    reg [15:0] confidence;
    reg [31:0] hits;
    reg [31:0] misses;
    reg [31:0] hold_cnt;

    //--------------------------------------------------------------------------
    // Combinational helpers
    //--------------------------------------------------------------------------

    wire [47:0] chunk_end    = sample_counter + {16'd0, n_r};
    wire        in_chunk     = (expected >= sample_counter) && (expected < chunk_end);
    wire [47:0] expected_off = expected - sample_counter;

    wire [31:0] fft32  = {16'd0, fft_r};
    wire [31:0] half32 = {16'd0, half_r};

    wire [15:0] thr_now  = cfg_threshold_q15[15] ? 16'd0 : cfg_threshold_q15;
    wire [15:0] relax_now = (thr_now * 4) / 5;

    wire [16:0] fft_plus_cp = {1'b0, fft_r} + {1'b0, cp_r};

    // Q1.15 blends, floor division (the model's blend_90_10 / blend_95_5 /
    // decay_90). Inputs are <= 65535, so the numerators fit 21 bits.
    wire [20:0] blend90 = (9 * {5'd0, confidence} + {5'd0, best}) / 10;
    wire [20:0] blend95 = (19 * {5'd0, confidence} + {5'd0, best}) / 20;
    wire [20:0] decay90 = (9 * {5'd0, confidence}) / 10;

    // The blends never exceed 65535 (a weighted mean of two 16-bit values);
    // expected_off is only used below n (32 bits); search_window bit 0 is
    // dropped by the halving.
    wire unused_ok = &{1'b0, blend90[20:16], blend95[20:16], decay90[20:16],
                       expected_off[47:32], cfg_search_window[0], 1'b0};

    assign corr_len                = fft_r;
    assign mon_sample_counter      = sample_counter;
    assign mon_state               = fs_state;
    assign mon_frame_number        = frame_number;
    assign mon_expected_boundary   = expected;
    assign mon_confidence          = confidence;
    assign mon_samples_to_boundary = (expected > sample_counter) ? (expected - sample_counter)
                                                                   : 48'd0;

    always @(posedge clk) begin
        if (rst) begin
            state                 <= S_IDLE;
            sample_counter        <= 48'd0;
            fft_r                 <= 16'd0;
            cp_r                  <= 16'd0;
            sym_r                 <= 16'd0;
            sub_r                 <= 16'd0;
            thr_r                 <= 16'd0;
            relax_r               <= 16'd0;
            acq_r                 <= 16'd0;
            loss_r                <= 16'd0;
            hold_r                <= 16'd0;
            half_r                <= 16'd0;
            n_r                   <= 32'd0;
            frame_samples         <= 48'd0;
            sym_samples           <= 48'd0;
            off                   <= 32'd0;
            end_r                 <= 32'd0;
            best                  <= 16'd0;
            best_off              <= 32'd0;
            fs_state              <= FS_SEARCHING;
            frame_number          <= 32'd0;
            expected              <= 48'd0;
            confidence            <= 16'd0;
            hits                  <= 32'd0;
            misses                <= 32'd0;
            hold_cnt              <= 32'd0;
            proc_done             <= 1'b0;
            corr_start            <= 1'b0;
            corr_offset           <= 32'd0;
            ev_valid              <= 1'b0;
            ev_sample_index       <= 48'd0;
            ev_frame_number       <= 32'd0;
            ev_confidence         <= 16'd0;
        end else begin
            proc_done  <= 1'b0;
            corr_start <= 1'b0;
            ev_valid   <= 1'b0;

            case (state)
                S_IDLE: begin
                    if (bs_apply) begin
                        expected   <= bs_offset;
                        confidence <= (bs_conf > SCORE_ONE) ? SCORE_ONE : bs_conf;
                        if (fs_state == FS_SEARCHING || fs_state == FS_HOLDOVER) begin
                            fs_state <= FS_ACQUIRING;
                            misses   <= 32'd0;
                            hits     <= 32'd1;
                        end
                    end else if (proc_start) begin
                        fft_r   <= cfg_fft_size;
                        cp_r    <= cfg_cp_length;
                        sym_r   <= cfg_symbols_per_subframe;
                        sub_r   <= cfg_subframes_per_frame;
                        thr_r   <= thr_now;
                        relax_r <= relax_now;
                        acq_r   <= cfg_lock_acquire_count;
                        loss_r  <= cfg_lock_loss_count;
                        hold_r  <= cfg_holdover_frames;
                        half_r  <= {1'b0, cfg_search_window[15:1]};
                        n_r     <= proc_n;
                        state   <= proc_skip ? S_FINISH : S_CFG0;
                    end
                end

                S_CFG0: begin
                    sym_samples <= sym_r * fft_plus_cp;
                    state       <= S_CFG1;
                end

                S_CFG1: begin
                    frame_samples <= sub_r * sym_samples;
                    state         <= S_DISPATCH;
                end

                S_DISPATCH: begin
                    case (fs_state)
                        FS_SEARCHING: begin
                            off   <= 32'd0;
                            state <= S_SRCH_CHK;
                        end

                        FS_ACQUIRING: begin
                            best     <= 16'd0;
                            best_off <= 32'd0;
                            if (in_chunk) begin
                                off   <= (expected_off[31:0] > half32) ? expected_off[31:0] - half32
                                                                       : 32'd0;
                                end_r <= (expected_off[31:0] + half32 > n_r) ? n_r
                                                                             : expected_off[31:0] + half32;
                            end else begin
                                off   <= 32'd0;
                                end_r <= n_r;
                            end
                            state <= S_ACQ_CHK;
                        end

                        FS_LOCKED: begin
                            best <= 16'd0;
                            if (in_chunk) begin
                                best_off <= expected_off[31:0];
                                off      <= (expected_off[31:0] > half32) ? expected_off[31:0] - half32
                                                                          : 32'd0;
                                end_r    <= (expected_off[31:0] + half32 + 32'd1 > n_r)
                                                ? n_r : expected_off[31:0] + half32 + 32'd1;
                                state    <= S_LCK_CHK;
                            end else begin
                                state <= S_FINISH;
                            end
                        end

                        default: begin  // FS_HOLDOVER
                            hold_cnt <= hold_cnt + 32'd1;
                            if (in_chunk) begin
                                if (expected_off[31:0] + fft32 <= n_r) begin
                                    corr_start  <= 1'b1;
                                    corr_offset <= expected_off[31:0];
                                    state       <= S_HLD_WAIT;
                                end else begin
                                    best  <= 16'd0;
                                    state <= S_HLD_EVAL;
                                end
                            end else begin
                                state <= S_HLD_TAIL;
                            end
                        end
                    endcase
                end

                //--- SEARCHING ----------------------------------------------
                S_SRCH_CHK: begin
                    if (off + fft32 <= n_r) begin
                        corr_start  <= 1'b1;
                        corr_offset <= off;
                        state       <= S_SRCH_WAIT;
                    end else begin
                        state <= S_FINISH;
                    end
                end

                S_SRCH_WAIT: begin
                    if (corr_done) begin
                        if (corr_ratio >= thr_r) begin
                            expected   <= sample_counter + {16'd0, off};
                            confidence <= corr_ratio;
                            fs_state   <= FS_ACQUIRING;
                            misses     <= 32'd0;
                            hits       <= 32'd1;
                            state      <= S_FINISH;
                        end else begin
                            off   <= off + {18'd0, fft_r[15:2]};  // F / 4
                            state <= S_SRCH_CHK;
                        end
                    end
                end

                //--- ACQUIRING ----------------------------------------------
                S_ACQ_CHK: begin
                    if (off + fft32 <= end_r && off < n_r) begin
                        corr_start  <= 1'b1;
                        corr_offset <= off;
                        state       <= S_ACQ_WAIT;
                    end else begin
                        state <= S_ACQ_EVAL;
                    end
                end

                S_ACQ_WAIT: begin
                    if (corr_done) begin
                        if (corr_ratio > best) begin
                            best     <= corr_ratio;
                            best_off <= off;
                        end
                        off   <= off + 32'd1;
                        state <= S_ACQ_CHK;
                    end
                end

                S_ACQ_EVAL: begin
                    state <= S_FINISH;
                    if (best >= thr_r) begin
                        expected <= sample_counter + {16'd0, best_off};
                        hits     <= hits + 32'd1;
                        if (hits + 32'd1 >= {16'd0, acq_r}) begin
                            fs_state        <= FS_LOCKED;
                            misses          <= 32'd0;
                            confidence      <= SCORE_ONE;
                            ev_valid        <= 1'b1;
                            ev_sample_index <= sample_counter;
                            ev_frame_number <= frame_number;
                            ev_confidence   <= SCORE_ONE;
                        end else begin
                            confidence <= blend90[15:0];
                        end
                    end else begin
                        misses <= misses + 32'd1;
                        if (misses + 32'd1 >= {16'd0, loss_r}) begin
                            fs_state   <= FS_SEARCHING;
                            hits       <= 32'd0;
                            misses     <= 32'd0;
                            confidence <= 16'd0;
                        end
                    end
                end

                //--- LOCKED -------------------------------------------------
                S_LCK_CHK: begin
                    if (off + fft32 <= n_r && off < end_r) begin
                        corr_start  <= 1'b1;
                        corr_offset <= off;
                        state       <= S_LCK_WAIT;
                    end else begin
                        state <= S_LCK_EVAL;
                    end
                end

                S_LCK_WAIT: begin
                    if (corr_done) begin
                        if (corr_ratio > best) begin
                            best     <= corr_ratio;
                            best_off <= off;
                        end
                        off   <= off + 32'd1;
                        state <= S_LCK_CHK;
                    end
                end

                S_LCK_EVAL: begin
                    state <= S_FINISH;
                    if (best >= relax_r) begin
                        confidence      <= blend95[15:0];
                        misses          <= 32'd0;
                        ev_valid        <= 1'b1;
                        ev_sample_index <= sample_counter;
                        ev_frame_number <= frame_number;
                        ev_confidence   <= blend95[15:0];
                        frame_number    <= frame_number + 32'd1;
                        expected        <= sample_counter + {16'd0, best_off} + frame_samples;
                    end else begin
                        misses <= misses + 32'd1;
                        if (misses + 32'd1 >= {16'd0, loss_r}) begin
                            fs_state <= FS_HOLDOVER;
                            hold_cnt <= 32'd0;
                        end
                    end
                end

                //--- HOLDOVER -----------------------------------------------
                S_HLD_WAIT: begin
                    if (corr_done) begin
                        best  <= corr_ratio;
                        state <= S_HLD_EVAL;
                    end
                end

                S_HLD_EVAL: begin
                    if (best >= thr_r) begin
                        fs_state        <= FS_LOCKED;
                        misses          <= 32'd0;
                        confidence      <= SCORE_ONE;
                        hits            <= 32'd1;
                        ev_valid        <= 1'b1;
                        ev_sample_index <= sample_counter;
                        ev_frame_number <= frame_number;
                        ev_confidence   <= SCORE_ONE;
                        state           <= S_FINISH;
                    end else begin
                        state <= S_HLD_TAIL;
                    end
                end

                S_HLD_TAIL: begin
                    state <= S_FINISH;
                    if (hold_cnt >= {16'd0, hold_r}) begin
                        fs_state   <= FS_SEARCHING;
                        hits       <= 32'd0;
                        misses     <= 32'd0;
                        confidence <= 16'd0;
                    end else begin
                        frame_number <= frame_number + 32'd1;
                        expected     <= expected + frame_samples;
                        confidence   <= decay90[15:0];
                    end
                end

                default: begin  // S_FINISH
                    sample_counter <= chunk_end;
                    proc_done      <= 1'b1;
                    state          <= S_IDLE;
                end
            endcase
        end
    end

endmodule
