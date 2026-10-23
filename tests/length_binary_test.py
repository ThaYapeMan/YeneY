# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Real bridge plus strict loopback Sonos/LMS: bounded source count mismatches."""
import http.client,math,os,struct,time,unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlsplit
import queue_binary_test as fixture
from length_test import alac,signal
from decoders_test import alac_parts

class Lengths(unittest.TestCase):
    def test_alac_eof(self):
        for rate in (44100,96000):
            for short in (1000,9000):
                values=signal(rate*4)
                if short==1000:body=alac(values,rate,declared=len(values)+short)
                else:
                    body=alac(values,rate);cookie,packets=alac_parts(body)
                    body=body[:-sum(map(len,packets[-3:]))]
                with patch.object(fixture,'alac',lambda:body):
                    h=fixture.Binary(codec='l',rates=(rate,),seconds=4)
                    try:
                        if short==9000:
                            h.until(lambda:len(h.packets('STMn'))==1)
                            time.sleep(.2);self.assertEqual(len(h.packets('STMn')),1)
                            self.assertFalse(h.items());self.assertIn('source-short',(h.base/'bridge.log').read_text())
                            print(f'PASS real binary ALAC {rate} large short: one STMn, Stop/release once, no retry loop',flush=True)
                            continue
                        h.until(lambda:len(h.packets('STMs'))==1)
                        item=h.items()[0];u=urlsplit(item['uri']);connection=http.client.HTTPConnection(u.netloc,timeout=8)
                        connection.request('GET',u.path+'?'+u.query);response=connection.getresponse();wave=response.read();connection.close()
                        target=rate if rate<=48000 else 48000;factor=rate//target
                        expected=(len(values)+1000+factor-1)//factor
                        self.assertEqual(len(wave),44+expected*6);self.assertEqual(int(item['resource']['size']),len(wave))
                        log=(h.base/'bridge.log').read_text();self.assertEqual(log.count('source-short padded '),1)
                        self.assertNotIn('relay fault=',log);self.assertFalse(h.packets('STMn'));self.assertEqual(len(h.sonos.actions('Stop')),0)
                        h.state(1,math.ceil(expected/target),'STOPPED')
                        h.until(lambda:len(h.packets('STMu'))==1)
                        self.assertFalse(h.packets('STMn'))
                        print(f'PASS real binary ALAC {rate} bounded short: exact WAV, one padding log, natural STMu, no Stop/STMn',flush=True)
                    finally:
                        try:h.close()
                        finally:
                            Path(f'/tmp/yeney-length-alac-{rate}-{short}.log').write_text((h.base/'bridge.log').read_text())
                            h.tmp.cleanup()
    def test_flac_eof(self):
        original=fixture.HTTP
        for delta in (1000,-1000,6000):
            def source(body,*args,**kwargs):
                data=bytearray(body);count=int.from_bytes(data[21:26],'big')&((1<<36)-1)
                count+=delta;data[21]=(data[21]&240)|((count>>32)&15);data[22:26]=(count&0xffffffff).to_bytes(4,'big')
                return original(bytes(data),*args,**kwargs)
            with patch.object(fixture,'HTTP',source):
                h=fixture.Binary(rates=(44100,),seconds=4)
                try:
                    if delta==6000:
                        h.until(lambda:len(h.packets('STMn'))==1);time.sleep(.2);self.assertEqual(len(h.packets('STMn')),1)
                        self.assertFalse(h.items());print('PASS real binary FLAC large short: one STMn and owned release',flush=True);continue
                    h.until(lambda:len(h.packets('STMs'))==1)
                    item=h.items()[0];u=urlsplit(item['uri']);c=http.client.HTTPConnection(u.netloc,timeout=8)
                    c.request('GET',u.path+'?'+u.query);response=c.getresponse();body=response.read();c.close()
                    self.assertEqual(len(body),44+(44100*4+delta)*6)
                    log=(h.base/'bridge.log').read_text();message='source-short padded' if delta>0 else 'source-long truncated'
                    self.assertEqual(log.count(message),1);self.assertFalse(h.packets('STMn'));self.assertNotIn('relay fault=',log)
                    print(f'PASS real binary FLAC delta={delta}: exact WAV, one reconciliation log, no Stop/STMn',flush=True)
                finally:
                    try:h.close()
                    finally:
                        Path(f'/tmp/yeney-length-flac-{delta}.log').write_text((h.base/'bridge.log').read_text())
                        h.tmp.cleanup()
if __name__=='__main__':unittest.main(verbosity=2)
