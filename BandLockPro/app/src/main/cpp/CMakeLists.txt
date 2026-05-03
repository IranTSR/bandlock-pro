cmake_minimum_required(VERSION 3.22.1)

project("qmi_tool")

# We build it as an EXECUTABLE, but name it libqmi_tool.so
# so that Android Gradle Plugin (AGP) will package it into the APK.
add_executable(qmi_tool
    qmi_tool.c
)

# Force the output name to have the 'lib' prefix and '.so' suffix
set_target_properties(qmi_tool PROPERTIES 
    OUTPUT_NAME "qmi_tool"
    PREFIX "lib"
    SUFFIX ".so"
)

find_library(log-lib log)
target_link_libraries(qmi_tool ${log-lib})
