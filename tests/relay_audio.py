# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import struct
import random

def wav(rate=44100,seconds=24,noise=False):
    pcm=random.Random(rate).randbytes(rate*4)*seconds if noise else struct.pack('<hh',1000,-1000)*(rate*seconds)
    return b'RIFF'+struct.pack('<I',36+len(pcm))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,1,2,rate,rate*4,4,16)+b'data'+struct.pack('<I',len(pcm))+pcm
