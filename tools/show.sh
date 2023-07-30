#! /bin/sh

INPUT=$1

for NAME in $(ls -l $INPUT/*.pro | awk '{print $9}' | sed "s/.pro$//g"); do
  
    echo "show " $NAME
    ./plua -i $NAME.pro -pprof $NAME.prof
    if [ $? -ne 0 ]; then
      echo "$NAME pprof fail"
      exit 1
    fi
    ./pprof --dot $NAME.prof > $NAME.dot
    if [ $? -ne 0 ]; then
      echo "$NAME dot fail"
      exit 1
    fi
    ./png -i $NAME.dot -png $NAME.png
    if [ $? -ne 0 ]; then
      echo "$NAME png fail"
      exit 1
    fi
    ./pprof --collapsed $NAME.prof > $NAME.fl
    if [ $? -ne 0 ]; then
      echo "$NAME collapsed fail"
      exit 1
    fi
    ./flamegraph.pl $NAME.fl > $NAME.svg
    if [ $? -ne 0 ]; then
      echo "$NAME svg fail"
      exit 1
    fi
    echo $NAME "ok"
done

echo "done!"
