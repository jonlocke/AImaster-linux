sudo socat -d -d \
  pty,raw,echo=0,link=/tmp/ttyA,mode=666 \
  pty,raw,echo=0,link=/tmp/ttyB,mode=666
