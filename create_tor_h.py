#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys

tor_bin = sys.argv[1]
tor_h = sys.argv[2]
with open(tor_bin, "rb") as tor_bin_stream, open(tor_h, "wb") as tor_h_stream:
    tor_h_stream.write("uint8_t TOR_EXE[] = { ".encode())
    first = True
    while True:
        b = tor_bin_stream.read(1)
        if not len(b):
            break
        val = hex(ord(b))
        if first:
            first = False
        else:
            tor_h_stream.write(", ".encode())
        tor_h_stream.write(val.encode())
    tor_h_stream.write(" };".encode())


