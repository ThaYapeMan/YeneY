"""One-off mechanical refactor proof against a Git revision; no device access."""
from pathlib import Path
import argparse
import re
import subprocess
ROOT=Path(__file__).resolve().parents[1]
NAMES=['milliseconds','speakerCommand','externalStop','fastPoll','ownershipIO','Impl','sourceInfo','relayEvents','received','beginSelection','selectPath','handoffComplete','publish','release','command','append','observe','run','shutdown','~Impl']
FILES=['queue_room.cpp','queue_relay.cpp','queue_path.cpp','queue_speaker.cpp','queue_lms_commands.cpp','queue_status.cpp']
LEX=r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
def masked(text):
    return re.sub(LEX,lambda m:''.join('\n' if c=='\n' else ' ' for c in m[0]),text)
def tokens(text):
    text=re.sub(LEX,lambda m:'' if m[0].startswith(('//','/*')) else m[0],text)
    return re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|\d+(?:\.\d*)?|\S',text)
def body(text,start):
    mask=masked(text);i=start;paren=0
    while i<len(text):
        if mask[i]=='(':paren+=1
        if mask[i]==')':paren-=1
        if mask[i]=='{' and not paren:break
        i+=1
    opening=i;depth=1;i+=1
    while depth:
        if mask[i]=='{':depth+=1
        if mask[i]=='}':depth-=1
        i+=1
    return opening,i

def compare(revision):
    old=subprocess.check_output(['git','show',revision+':queue_room.cpp'],cwd=ROOT,text=True)
    begin=old.index('struct QueueRoom::Impl {');end=old.index('\n};',begin)+3
    header=old[begin:end]; edits=[]
    for name in NAMES:
        match=re.search(r'^    (?! )[^\n]*?(?<![\w~])'+re.escape(name)+r'\(',old[begin:end],re.M)
        assert match,name
        start=begin+match.start();opening,finish=body(old,start)
        matches=[]
        for filename in FILES:
            new=(ROOT/filename).read_text()
            found=re.search(r'^.*?QueueRoom::Impl::'+re.escape(name)+r'\(',new,re.M)
            if found:matches.append((new,found.start()))
        assert len(matches)==1,(name,'missing/duplicate definition')
        new,start_new=matches[0];opening_new,finish_new=body(new,start_new)
        assert tokens(old[opening:finish])==tokens(new[opening_new:finish_new]),('method changed',name)
        signature=old[start:opening].rstrip()
        if name=='Impl':
            init=signature.index('\n        :')
            new_signature=new[start_new:opening_new]
            assert tokens(signature[init:])==tokens(new_signature[re.search(r'\n\s*:',new_signature).start():]),'constructor initializer order changed'
            signature=signature[:init]
        edits.append((start-begin,finish-begin,signature+';'))
    for start,finish,replacement in sorted(edits,reverse=True):header=header[:start]+replacement+header[finish:]
    new_header=(ROOT/'queue_room_internal.h').read_text();start=new_header.index('struct QueueRoom::Impl {');finish=new_header.index('\n};',start)+3
    assert tokens(header)==tokens(new_header[start:finish]),'shared state/declaration order changed'
    print('PASS queue refactor: 20 method bodies, constructor initializers and shared state/declaration order token-identical to '+revision+'; worker/locks/log literals unchanged')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline',default='c3baa3d');compare(p.parse_args().baseline)
