pushd crankshaft
make lib
popd
pushd build
rm webmud
cmake --build .
popd
