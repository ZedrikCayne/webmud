pushd crankshaft >/dev/null
make library >/dev/null
popd >/dev/null
pushd build >/dev/null
cmake --build . >/dev/null
popd >/dev/null
