pushd crankshaft
make library
popd
pushd build
cmake --build .
popd
