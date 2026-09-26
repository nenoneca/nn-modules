# nn_infer — portable core + selectable PAL.
# Consumers add the sources directly (same pattern as nn_osal):
#   NN_INFER_PAL = stub | byai_dlr
if(NOT DEFINED NN_INFER_PAL)
    set(NN_INFER_PAL stub)
endif()
set(NN_INFER_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/pal/${NN_INFER_PAL}/pal_${NN_INFER_PAL}.c
    PARENT_SCOPE)
set(NN_INFER_INCLUDE ${CMAKE_CURRENT_LIST_DIR}/include PARENT_SCOPE)
