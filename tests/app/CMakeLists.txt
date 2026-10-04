gateway_add_test(gateway_core_test
    SOURCES GatewayCoreTest.cpp
    LIBS gw_app
    LABELS "unit;app;concurrency"
)
