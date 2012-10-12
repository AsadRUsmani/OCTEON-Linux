#!/bin/bash
for P in `ls ../xen-patches-2.6.32-2/6*.patch1 | sort`
do
    patch -p1 -s -i $P
    if [ $? = 0 ]; then
        echo $P applied
    else
        echo "Error processing "$P
        exit 1
    fi
done
