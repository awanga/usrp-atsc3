# Building UHD 3.15 on a current toolchain

The N2x0 hardware path needs UHD 3.15.0.0, which predates GCC 15, Boost 1.90
and CMake 4. Verified on Ubuntu 26.04 (GCC 15.2, Boost 1.90, CMake 4.2,
Python 3.14, GNU Radio 3.10.12) with the changes below; UHD is installed in
`/opt/uhd` and the project finds it through `CMAKE_PREFIX_PATH`.

## Source changes

Apply to a clean `v3.15.0.0` checkout (the patch is kept outside this
repository because it is a modification of GPL-3.0 code):

| Problem | Change |
|---|---|
| CMake 4 rejects `cmake_minimum_required(VERSION 3.5.1)` | `3.5.1...3.27` |
| No `boost_system` library or CMake component in Boost 1.89+ | drop `system` from the required components |
| `"3.14.4" >= "3.5"` is a string comparison in the Python version check | compare version tuples |
| `boost::asio::io_service`, `resolver::query`, `address_v4::from_string`, `to_ulong` removed (Boost 1.87) | `io_context`, `resolve(proto, host, port)`, `make_address_v4`, `to_uint` |
| `<boost/bind.hpp>` no longer exports `_1`..`_9`; `boost::math::round/sign` need their own headers | `cmake/uhd_boost_compat.hpp`, force-included |
| `<boost/timer.hpp>` is deprecated | define `BOOST_TIMER_ENABLE_DEPRECATED` in `converter_benchmark.cpp` |
| `lmx2592.cpp` uses `std::array` without `<array>` | add the include |
| `always_inline` on virtual methods fails in `zero_copy_flow_ctrl.cpp` (GCC 15) | plain `inline` |
| `property<T>()` constructors and destructors with a template-id fail under C++20 (installed headers, so projects building with `-Werror` break) | drop the `<T>` |

## Configure, build, install

```bash
cmake ../host \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_INSTALL_PREFIX=/opt/uhd -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-include $PWD/../host/cmake/uhd_boost_compat.hpp -Wno-deprecated-declarations" \
  -DENABLE_E100=OFF -DENABLE_E300=OFF -DENABLE_X300=OFF -DENABLE_B200=OFF \
  -DENABLE_USRP1=OFF -DENABLE_OCTOCLOCK=OFF -DENABLE_MPMD=OFF \
  -DENABLE_TESTS=OFF -DENABLE_PYTHON_API=OFF -DENABLE_MANUAL=OFF \
  -DENABLE_DOXYGEN=OFF -DENABLE_MAN_PAGES=OFF
make -j6 && make install
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/uhd
```

Only the N2x0 (USRP2 family) and the examples and utilities are enabled.
FPGA and firmware images for the N2x0 are fetched separately with
`/opt/uhd/bin/uhd_images_downloader`.
