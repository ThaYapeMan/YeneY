"""Compare eight complete real-binary 44.1 kHz queue WAV items (loopback only)."""
import argparse
import hashlib
import http.client
from pathlib import Path
from urllib.parse import urlsplit
from queue_binary_test import Binary

def album(binary):
    host=Binary(rates=(44100,)*8,seconds=8,binary=Path(binary).resolve(),queue_title_prefix="")
    bodies=[]
    try:
        for index in range(8):
            title=host.titles[index]
            host.until(lambda:any(i.get('metadata',{}).get('title')==title for i in host.items()))
            ordinal=next(n+1 for n,i in enumerate(host.items()) if i['metadata']['title']==title)
            url=urlsplit(host.items()[ordinal-1]['uri'])
            host.state(ordinal)
            host.until(lambda:len(host.packets('STMs'))>=index+1)
            connection=http.client.HTTPConnection(url.netloc,timeout=10)
            try:
                connection.request('GET',url.path+'?'+url.query)
                response=connection.getresponse();body=response.read()
                assert response.status==200 and body[:4]==b'RIFF' and len(body)==44+44100*8*6
                bodies.append(body)
            finally:connection.close()
    finally:host.close()
    return bodies

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before',required=True);parser.add_argument('--after',required=True)
    args=parser.parse_args();before=album(args.before);after=album(args.after)
    assert before==after,'44.1 kHz WAV bytes changed'
    for index,body in enumerate(before,1):
        print('PASS complete 44.1 kHz WAV item '+str(index)+': byte-identical '+str(len(body))+' bytes SHA256 '+hashlib.sha256(body).hexdigest())
