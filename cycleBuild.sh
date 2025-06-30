while true
do
    sleep 1
    ./build.sh 2>~/a.out
    vi ~/a.out
done
