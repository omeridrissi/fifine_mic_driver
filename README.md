# FIFINE usb microphone driver

First attempt at making a real device driver for the linux kernel

## Building

```sh
# Compile
make

# Clean
make clean
```

## Use

The driver outputs a pure uncompressed audio stream into userspace.

```sh
# Get the audio from stream
sudo cat /dev/fifine1 > output.dat

# Play the pure uncompressed audio stream
ffplay -f s16le -ar 48000 -i output.dat
```
