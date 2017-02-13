#!/bin/bash

env
echo "-----"

if [ ! -f build/src/keepassxc ]; then
  mkdir -p build
  cd build
  cmake -DWITH_XC_AUTOTYPE=ON -DWITH_XC_HTTP=ON -DCMAKE_BUILD_TYPE=Release ..
  make -j8
  cd -
fi

if [ ! -f /home/${X_USER}/.local/usr/local/bin/keepassxc ]; then
  cd build
  make DESTDIR=/home/${X_USER}/.local install

  echo "${X_USER}:x:${X_UID}:${X_GID}:${X_USER},,,:/home/${X_USER}:/bin/bash" >> /etc/passwd
  echo "${X_USER}:x:${X_UID}:" >> /etc/group
  echo "${X_USER} ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/${X_USER}
  chmod 0440 /etc/sudoers.d/${X_USER}
  chown ${X_UID}:${X_GID} -R /home/${X_USER}
fi

su - ${X_USER} -c "export PATH=.local/usr/local/bin:/opt/qt58/lib:$PATH; export LD_LIBRARY_PATH=/opt/qt58/lib; keepassxc"
