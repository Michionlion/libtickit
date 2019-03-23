#!/bin/bash

wget http://www.leonerd.org.uk/code/libtermkey/libtermkey-0.22.tar.gz
tar -xzvf libtermkey-0.22.tar.gz
(
\cd libtermkey-0.22 || exit
make PREFIX=/usr all
sudo make PREFIX=/usr install
)
