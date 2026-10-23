# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Original deterministic 24-bit PCM and ALAC escape-packet fixture writer."""
import struct
import numpy as np
from decoders_test import mp4

def pcm(rate,seconds,start=0):
    rng=np.random.default_rng(20261023+rate+start)
    n=np.arange(rate*seconds)+start
    samples=.15*np.sin(2*np.pi*1000*n/rate)+rng.normal(0,.035,len(n))
    left=np.rint(samples*2**23).astype('<i4')
    right=np.rint((.14*np.cos(2*np.pi*19000*n/rate)+rng.normal(0,.03,len(n)))*2**23).astype('<i4')
    return np.column_stack((left,right))

def raw24(values):
    u=values.astype('<i4').view(np.uint8).reshape(-1,4)
    return u[:,:3].tobytes()

def wave(values,rate):
    body=raw24(values)
    return b'RIFF'+struct.pack('<I',len(body)+36)+b'WAVEfmt '+struct.pack('<IHHIIHH',16,1,2,rate,rate*6,6,24)+b'data'+struct.pack('<I',len(body))+body

def alac(values,rate):
    packets=[]
    for i in range(0,len(values),4096):
        block=values[i:i+4096]
        payload=block.astype('>i4').view(np.uint8).reshape(-1,4)[:,1:].tobytes()
        # CPE, instance 0, reserved 12, explicit size, no shifted bytes, escape.
        header=int('001'+'0000'+'0'*12+'1001'+f'{len(block):032b}',2)
        bits=55+len(payload)*8+3;padding=(-bits)%8
        binary=((header<<(len(payload)*8+3))|(int.from_bytes(payload,'big')<<3)|7)<<padding
        packets.append(binary.to_bytes((bits+padding)//8,'big'))
    cookie=struct.pack('!I',4096)+bytes([0,24,40,10,14,2])+bytes(10)+struct.pack('!I',rate)
    return mp4(cookie,packets,len(values),complete=True)
