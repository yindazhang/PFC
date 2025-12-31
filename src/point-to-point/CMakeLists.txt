set(mpi_sources)
set(mpi_headers)
set(mpi_libraries)

if(${ENABLE_MPI})
  set(mpi_sources
      model/point-to-point-remote-channel.cc
  )
  set(mpi_headers
      model/point-to-point-remote-channel.h
  )
  set(mpi_libraries
      ${libmpi}
      MPI::MPI_CXX
  )
endif()

build_lib(
  LIBNAME point-to-point
  SOURCE_FILES
    ${mpi_sources}
    helper/point-to-point-helper.cc
    model/point-to-point-channel.cc
    model/point-to-point-net-device.cc
    model/point-to-point-queue.cc
    model/rdma-queue-pair.cc
    model/switch-node.cc
    model/hpcc-header.cc
    model/ppp-header.cc
    model/bth-header.cc
    model/pfc-header.cc
    model/packet-tag.cc
  HEADER_FILES
    ${mpi_headers}
    helper/point-to-point-helper.h
    model/point-to-point-channel.h
    model/point-to-point-net-device.h
    model/point-to-point-queue.h
    model/rdma-queue-pair.h
    model/switch-node.h
    model/hpcc-header.h
    model/ppp-header.h
    model/bth-header.h
    model/pfc-header.h
    model/packet-tag.h
  LIBRARIES_TO_LINK ${libnetwork}
                    ${mpi_libraries}
  TEST_SOURCES test/point-to-point-test.cc
)
