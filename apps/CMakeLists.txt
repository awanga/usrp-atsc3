# apps/ - Applications
# atsc3_rx.grc, scanner.py, metrics_server.py

# Install GRC flowgraphs
set(GRC_FLOWGRAPHS
    # atsc3_rx.grc
)

# Install Python applications
set(PYTHON_APPS
    # scanner.py
    # metrics_server.py
)

# if(GRC_FLOWGRAPHS)
#     install(FILES ${GRC_FLOWGRAPHS}
#         DESTINATION share/gnuradio/grc/blocks
#     )
# endif()

# if(PYTHON_APPS)
#     install(PROGRAMS ${PYTHON_APPS}
#         DESTINATION bin
#     )
# endif()
