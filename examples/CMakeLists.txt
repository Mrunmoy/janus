# Examples
add_executable(sensor_pipeline sensor_pipeline.c)
target_link_libraries(sensor_pipeline PRIVATE cfuture)
set_target_properties(sensor_pipeline PROPERTIES C_STANDARD 11)
