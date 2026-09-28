# Regression tests for https://github.com/shadow/shadow/issues/3701
#
# These verify that the upload path buffers packets in an uplink CoDel router
# instead of using the socket send buffer (and `EWOULDBLOCK`) as the upload rate
# limiter.
add_shadow_tests(
    BASENAME uplink-aqm-low-bdp
    LOGLEVEL info
    POST_CMD "python3 ${CMAKE_CURRENT_SOURCE_DIR}/verify.py --min-sent 1000000 --max-elapsed 2.0"
)
add_shadow_tests(
    BASENAME uplink-aqm-high-bdp
    LOGLEVEL info
    POST_CMD "python3 ${CMAKE_CURRENT_SOURCE_DIR}/verify.py --min-sent 20000000 --max-elapsed 2.5"
)
