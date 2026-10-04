find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(MOSQUITTO_EXECUTABLE mosquitto)
find_program(MOSQUITTO_PUB_EXECUTABLE mosquitto_pub)
find_program(MOSQUITTO_SUB_EXECUTABLE mosquitto_sub)
if(Python3_Interpreter_FOUND AND MOSQUITTO_EXECUTABLE AND MOSQUITTO_PUB_EXECUTABLE)
    add_test(NAME serial_output_e2e
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/serial_output.py
                $<TARGET_FILE:gateway>)
    set_tests_properties(serial_output_e2e PROPERTIES LABELS "e2e" TIMEOUT 90)
    if(MOSQUITTO_SUB_EXECUTABLE)
        add_test(NAME architecture_e2e
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/architecture_e2e.py
                    $<TARGET_FILE:gateway>)
        set_tests_properties(architecture_e2e PROPERTIES LABELS "e2e" TIMEOUT 90)
    endif()
    if(TARGET node-sim AND MOSQUITTO_SUB_EXECUTABLE)
        add_test(NAME sr_transport_e2e
            COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/sr_transport.py
                    $<TARGET_FILE:gateway> $<TARGET_FILE:node-sim>)
        set_tests_properties(sr_transport_e2e PROPERTIES LABELS "e2e" TIMEOUT 60)
    endif()
endif()
