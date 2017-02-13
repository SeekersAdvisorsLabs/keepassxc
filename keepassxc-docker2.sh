#!/bin/bash

[ ! -d ~/Documents/keepass ] && mkdir -p ~/Documents/keepass

docker run --rm -it \
 -e DISPLAY=$DISPLAY \
 -v /tmp/.X11-unix:/tmp/.X11-unix \
 -v /etc/localtime:/etc/localtime \
 -v ~/Documents/keepass:/home/keepassxreboot/kees \
 keepassxreboot/keepassxc:2.1.1bis
