#!/bin/bash

echo "
DISPLAY=$DISPLAY
UID=$(id -u)
GID=$(id -g)
USER=$USER" > .env

docker-compose up -d
