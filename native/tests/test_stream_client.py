"""Compile the actual raw client against a bounded host lwIP mock, in both input profiles."""
from pathlib import Path
import os, shutil, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
files = {
'lwip/prot/tcp.h': '#pragma once\n#include <cstdint>\nstruct tcp_hdr { uint32_t seqno; };\n',
'lwip/err.h': '''#pragma once
using err_t=int;
constexpr err_t ERR_OK=0, ERR_MEM=-1, ERR_RST=-2, ERR_ABRT=-3;
''',
'lwip/ip_addr.h': '''#pragma once
struct ip_addr_t { unsigned value; };
#define IPADDR_TYPE_V4 0
#define IP_ADDR4(p,a,b,c,d) ((p)->value = 0)
''',
'lwip/pbuf.h': '''#pragma once
#include <cstdint>
struct pbuf { pbuf* next=nullptr; void* payload=nullptr; uint16_t len=0, tot_len=0; };
unsigned pbuf_free(pbuf*);
''',
'lwip/tcp.h': '''#pragma once
#include <cstdint>
#include <vector>
#include "err.h"
#include "ip_addr.h"
struct pbuf;
struct tcp_pcb {
 void* arg=nullptr;
 err_t (*recv)(void*,tcp_pcb*,pbuf*,err_t)=nullptr;
 void (*error)(void*,err_t)=nullptr;
 err_t (*sent)(void*,tcp_pcb*,uint16_t)=nullptr;
 err_t (*connected)(void*,tcp_pcb*,err_t)=nullptr;
 uint16_t remote_port=8081; int state=4; uint32_t rcv_nxt=1000;
 bool aborted=false;
 unsigned acked=0, outputs=0;
 std::vector<uint8_t> writes;
};
constexpr int ESTABLISHED=4;
constexpr unsigned TCP_WRITE_FLAG_COPY=1;
tcp_pcb* tcp_new_ip_type(int);
void tcp_arg(tcp_pcb*,void*);
void tcp_recv(tcp_pcb*,err_t(*)(void*,tcp_pcb*,pbuf*,err_t));
void tcp_err(tcp_pcb*,void(*)(void*,err_t));
void tcp_abort(tcp_pcb*);
void tcp_sent(tcp_pcb*,err_t(*)(void*,tcp_pcb*,uint16_t));
void tcp_nagle_disable(tcp_pcb*);
err_t tcp_connect(tcp_pcb*,const ip_addr_t*,uint16_t,err_t(*)(void*,tcp_pcb*,err_t));
err_t tcp_write(tcp_pcb*,const void*,uint16_t,unsigned);
err_t tcp_output(tcp_pcb*);
void tcp_recved(tcp_pcb*,uint16_t);
''',
'pico/time.h': '''#pragma once
#include <cstdint>
extern uint64_t fake_us;
inline uint64_t time_us_64() { return fake_us; }
inline uint32_t time_us_32() { return static_cast<uint32_t>(fake_us); }
''',
'review.cpp': r'''#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "stream_client.hpp"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include "lwip/prot/tcp.h"
#include "tcp_ingress_hook.h"
uint64_t fake_us=0;
err_t write_result=ERR_OK, output_result=ERR_OK;
unsigned freed=0;
std::vector<std::unique_ptr<tcp_pcb>> sockets;
tcp_pcb* tcp_new_ip_type(int) { sockets.emplace_back(new tcp_pcb); return sockets.back().get(); }
void tcp_arg(tcp_pcb* p,void* v){p->arg=v;}
void tcp_recv(tcp_pcb* p,err_t(*f)(void*,tcp_pcb*,pbuf*,err_t)){p->recv=f;}
void tcp_err(tcp_pcb* p,void(*f)(void*,err_t)){p->error=f;}
void tcp_sent(tcp_pcb* p,err_t(*f)(void*,tcp_pcb*,uint16_t)){p->sent=f;}
void tcp_abort(tcp_pcb* p){assert(!p->aborted);p->aborted=true;}
void tcp_nagle_disable(tcp_pcb*){}
err_t tcp_connect(tcp_pcb* p,const ip_addr_t*,uint16_t,err_t(*f)(void*,tcp_pcb*,err_t)){p->connected=f;return ERR_OK;}
err_t tcp_write(tcp_pcb* p,const void* v,uint16_t n,unsigned){assert(!p->aborted);if(write_result!=ERR_OK)return write_result;auto* b=static_cast<const uint8_t*>(v);p->writes.insert(p->writes.end(),b,b+n);return ERR_OK;}
err_t tcp_output(tcp_pcb* p){assert(!p->aborted);++p->outputs;return output_result;}
void tcp_recved(tcp_pcb* p,uint16_t n){assert(!p->aborted);p->acked+=n;}
unsigned pbuf_free(pbuf*){++freed;return 1;}
using native_badge::StreamClient;
using native_badge::ReceivedFrame;
uint8_t second_input[StreamClient::jpeg_capacity]{};
void init(StreamClient& s,uint8_t* first) { s.init(first,6,second_input); }

void poll(StreamClient& s,uint32_t t){fake_us=uint64_t(t)*1000;s.poll(t,true);}
tcp_pcb* connect(StreamClient& s,uint32_t t){poll(s,t);auto* p=sockets.back().get();assert(p->connected(p->arg,p,ERR_OK)==ERR_OK);assert(s.connected());return p;}
std::vector<uint8_t> message(unsigned kind,unsigned n,uint32_t len){std::vector<uint8_t> out(16+len);streamer::pack_header(out.data(),kind,n,kind==1?320:0,len,123);for(uint32_t i=0;i<len;i++)out[16+i]=static_cast<uint8_t>(i+n);return out;}
err_t receive(tcp_pcb* p,const uint8_t* bytes,size_t n,size_t split=0){assert(n<=65535);pbuf first,second;first.payload=const_cast<uint8_t*>(bytes);first.len=first.tot_len=static_cast<uint16_t>(n);if(split&&split<n){first.len=static_cast<uint16_t>(split);first.next=&second;second.payload=const_cast<uint8_t*>(bytes+split);second.len=second.tot_len=static_cast<uint16_t>(n-split);}auto before=freed;auto r=p->recv(p->arg,p,&first,ERR_OK);assert(freed==before+1);return r;}
int main(){
 // Every possible header split, pbuf chaining, STATUS between frames, immutable in-use JPEG.
 for(unsigned split=1;split<16;split++){
  uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,0);assert(p->writes.size()==8&&p->writes[6]==64&&p->writes[7]==0);
  auto f=message(1,7,97);assert(receive(p,f.data(),split)==ERR_OK);assert(!s.frame_ready());assert(receive(p,f.data()+split,f.size()-split,13)==ERR_OK);assert(p->acked==f.size()&&s.frames==1&&s.frame_ready());
  ReceivedFrame got;assert(s.take_frame(got)&&got.length==97&&got.sequence==7);auto st=message(2,1,512);assert(receive(p,st.data(),st.size(),47)==ERR_OK);assert(s.statuses==1&&!std::memcmp(jpeg,f.data()+16,97));assert(p->writes.size()==8);s.frame_done();assert(p->writes.size()==9&&p->writes.back()=='C');s.close();assert(p->aborted);
 }
 // Legacy cap overrun drains without writing out of bounds, restores exactly one credit, then reads following message.
 {std::vector<uint8_t> guarded(StreamClient::jpeg_capacity+16,0xab);StreamClient s;init(s,guarded.data()+8);auto* p=connect(s,100);auto big=message(1,1,65537);for(size_t off=0;off<big.size();){size_t n=std::min<size_t>(137,big.size()-off);assert(receive(p,big.data()+off,n,13)==ERR_OK);off+=n;}assert(s.oversized==1&&!s.frame_ready()&&s.frames==0&&p->writes.size()==9);for(auto b:guarded)assert(b==0xab);auto f=message(1,2,0);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got)&&got.length==0);s.frame_done();s.close();}
 // Active decoder pins JPEG across remote error/reconnect. Completion credits no defunct connection.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,1000);auto f=message(1,1,71);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got));auto count=sockets.size();auto error=p->error;auto arg=p->arg;p->aborted=true;error(arg,ERR_RST);poll(s,1300);assert(sockets.size()==count&&!s.connected());assert(!std::memcmp(jpeg,f.data()+16,71));s.frame_done();assert(p->writes.size()==8);auto* q=connect(s,1301);assert(q!=p&&q->writes.size()==8);s.close();}
 // Unexpected extra frame while decoder holds storage aborts and retains decoder gate.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,2000);auto f=message(1,1,71);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got));assert(receive(p,f.data(),f.size())==ERR_ABRT);assert(p->aborted&&s.errors==1);auto count=sockets.size();poll(s,2300);assert(sockets.size()==count);s.frame_done();connect(s,2301);s.close();}
 // Protocol bounds prevent STATUS overflow. HELLO allocation errors have a finite setup timeout.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,3000);auto bad=message(2,1,513);assert(receive(p,bad.data(),bad.size())==ERR_ABRT&&s.errors==1);}
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);write_result=ERR_MEM;poll(s,4000);auto* p=sockets.back().get();assert(p->connected(p->arg,p,ERR_OK)==ERR_OK);assert(!s.connected());poll(s,4999);assert(!p->aborted);poll(s,5000);assert(p->aborted&&s.errors==1);write_result=ERR_OK;}
 // tcp_output's transient failure is retried next poll without duplicate HELLO or credit.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);output_result=ERR_MEM;auto* p=connect(s,6000);assert(p->writes.size()==8&&p->outputs==1);poll(s,6001);assert(p->writes.size()==8&&p->outputs==2);output_result=ERR_OK;poll(s,6002);assert(p->outputs==3);poll(s,6003);assert(p->outputs==3);s.close();}
// STATUS payloads remain bounded and opaque: streaming needs no control parser.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,8000);assert(p->sent);fake_us+=100;p->sent(p->arg,p,8);
  const char json[]="{\"fps\":34}";auto st=message(2,1,sizeof(json));assert(receive(p,st.data(),st.size(),9)==ERR_OK);assert(s.statuses==1);
  auto f=message(1,11,20);assert(receive(p,f.data(),f.size(),11)==ERR_OK);ReceivedFrame got;assert(s.take_frame(got)&&got.session==1&&got.trace.credit_source_seq==0&&got.trace.credit_ack_us==fake_us);s.frame_done();fake_us+=100;p->sent(p->arg,p,1);
  auto f2=message(1,12,20);assert(receive(p,f2.data(),f2.size())==ERR_OK);assert(s.take_frame(got)&&got.trace.credit_source_seq==11&&got.trace.credit_ack_us==fake_us);s.frame_done();s.close();assert(p->sent==nullptr);
 }
 // Actual C hook filters null/other-port/other-state/other-PCB/no-payload, resets each credit.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,9000);tcp_hdr hdr{2000};pbuf pkt;pkt.tot_len=100;
  tcp_pcb other;fake_us+=10;assert(native_tcp_ingress_hook(nullptr,&hdr,&pkt)==ERR_OK);assert(native_tcp_ingress_hook(p,nullptr,&pkt)==ERR_OK);assert(native_tcp_ingress_hook(p,&hdr,nullptr)==ERR_OK);assert(native_tcp_ingress_hook(&other,&hdr,&pkt)==ERR_OK);
  p->remote_port=8082;native_tcp_ingress_hook(p,&hdr,&pkt);p->remote_port=8081;p->state=2;native_tcp_ingress_hook(p,&hdr,&pkt);p->state=ESTABLISHED;pkt.tot_len=0;native_tcp_ingress_hook(p,&hdr,&pkt);pkt.tot_len=100;
  native_tcp_ingress_hook(p,&hdr,&pkt);fake_us+=10;hdr.seqno=3000;native_tcp_ingress_hook(p,&hdr,&pkt);fake_us+=10;hdr.seqno=800;pkt.tot_len=200;native_tcp_ingress_hook(p,&hdr,&pkt);fake_us+=30;hdr.seqno=900;native_tcp_ingress_hook(p,&hdr,&pkt);
  auto f=message(1,11,20);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got));auto i=got.trace.ingress;assert(i.segments==4&&i.ahead==2&&i.behind==2&&i.max_ahead_bytes==2000&&i.cover_events==1&&i.cover_wait_max_us==50&&i.pending_ahead_us==0);s.frame_done();
  auto f2=message(1,12,20);assert(receive(p,f2.data(),f2.size())==ERR_OK);assert(s.take_frame(got)&&got.trace.ingress.segments==0);s.frame_done();s.close();
 }
 // An output attempt that failed has not opened the ingress observation interval yet.
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);output_result=ERR_MEM;auto* p=connect(s,10000);tcp_hdr hdr{1000};pbuf pkt;pkt.tot_len=100;native_tcp_ingress_hook(p,&hdr,&pkt);output_result=ERR_OK;poll(s,10001);auto f=message(1,1,20);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got)&&got.trace.ingress.segments==0);s.frame_done();s.close();}

#if NATIVE_JPEG_PSRAM
 // A leased input and the next receiving/ready input stay independent. Late
 // completion never duplicates an early credit, including an allocation retry.
 {std::vector<uint8_t> a(StreamClient::jpeg_capacity),b(StreamClient::jpeg_capacity);
  StreamClient s;s.init(a.data(),6,b.data());auto* p=connect(s,11000);
  auto f1=message(1,1,97);assert(receive(p,f1.data(),f1.size())==ERR_OK);ReceivedFrame one;assert(s.take_frame(one));
  assert(s.frame_done(one)&&s.late_credits==1&&p->writes.size()==9);assert(!s.frame_done(one));
  auto f2=message(1,2,101);assert(receive(p,f2.data(),f2.size())==ERR_OK);ReceivedFrame two;assert(s.take_frame(two));
  std::vector<uint8_t> stable(two.jpeg,two.jpeg+two.length);
  assert(s.prefetch_credit(two)&&s.early_credits==1&&p->writes.size()==10);assert(!s.prefetch_credit(two));
  auto f3=message(1,3,103);assert(receive(p,f3.data(),29)==ERR_OK);assert(!std::memcmp(two.jpeg,stable.data(),stable.size()));
  assert(receive(p,f3.data()+29,f3.size()-29,17)==ERR_OK&&s.frame_ready());
  ReceivedFrame three;assert(!s.take_frame(three));assert(!s.frame_done(one)&&p->writes.size()==10);
  assert(s.frame_done(two)&&p->writes.size()==10);assert(s.take_frame(three)&&three.jpeg!=two.jpeg&&three.sequence==3);
  assert(three.trace.credit_source_seq==2&&!std::memcmp(two.jpeg,stable.data(),stable.size()));
  write_result=ERR_MEM;assert(s.prefetch_credit(three)&&p->writes.size()==10);assert(!s.prefetch_credit(three));
  assert(s.frame_done(three)&&p->writes.size()==10);write_result=ERR_OK;poll(s,11001);assert(p->writes.size()==11);poll(s,11002);assert(p->writes.size()==11);
  auto f4=message(1,4,105);assert(receive(p,f4.data(),f4.size())==ERR_OK);ReceivedFrame four;assert(s.take_frame(four));
  stable.assign(four.jpeg,four.jpeg+four.length);assert(s.prefetch_credit(four));
  auto f5=message(1,5,107);assert(receive(p,f5.data(),31)==ERR_OK);s.close();
  auto count=sockets.size();poll(s,11300);assert(sockets.size()==count&&!std::memcmp(four.jpeg,stable.data(),stable.size()));
  assert(!s.frame_done(three)&&!s.prefetch_credit(four));assert(s.frame_done(four));
  auto* q=connect(s,11301);assert(q!=p);assert(receive(q,f5.data(),f5.size())==ERR_OK);ReceivedFrame five;assert(s.take_frame(five)&&five.session!=four.session);
  assert(!s.frame_done(four)&&q->writes.size()==8);assert(s.frame_done(five)&&q->writes.size()==9);s.close();
 }
 // An old sender's oversize drain replaces its consumed credit, but does not
 // grant again when the earlier decoder subsequently completes.
 {std::vector<uint8_t> a(StreamClient::jpeg_capacity+16,0xab),b(StreamClient::jpeg_capacity+16,0xcd);
  StreamClient s;s.init(a.data()+8,6,b.data()+8);auto* p=connect(s,12000);auto f=message(1,10,91);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame lease;assert(s.take_frame(lease));assert(s.prefetch_credit(lease));
  auto big=message(1,11,65537);for(size_t off=0;off<big.size();){auto n=std::min<size_t>(137,big.size()-off);assert(receive(p,big.data()+off,n)==ERR_OK);off+=n;}
  assert(s.oversized==1&&p->writes.size()==10&&s.frame_done(lease)&&p->writes.size()==10);
  for(unsigned i=0;i<8;++i){assert(a[i]==0xab&&a[a.size()-1-i]==0xab&&b[i]==0xcd&&b[b.size()-1-i]==0xcd);}
  auto next=message(1,12,93);assert(receive(p,next.data(),next.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got));assert(s.frame_done(got)&&p->writes.size()==11);s.close();
 }
 // A reserved early credit which tcp_write refused is not permission for a
 // peer to send another frame. Abort without touching the live decoder input.
 {std::vector<uint8_t> a(StreamClient::jpeg_capacity),b(StreamClient::jpeg_capacity);StreamClient s;s.init(a.data(),6,b.data());auto* p=connect(s,12500);
  auto f=message(1,1,91);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame lease;assert(s.take_frame(lease));std::vector<uint8_t> stable(lease.jpeg,lease.jpeg+lease.length);
  write_result=ERR_MEM;assert(s.prefetch_credit(lease));auto next=message(1,2,93);assert(receive(p,next.data(),next.size())==ERR_ABRT);assert(!std::memcmp(lease.jpeg,stable.data(),stable.size()));write_result=ERR_OK;assert(s.frame_done(lease));
 }
 // Two independent physical ranges are mandatory; overlap cannot connect.
 {uint8_t a[StreamClient::jpeg_capacity]{};StreamClient s;s.init(a,6,a+1);auto count=sockets.size();poll(s,13000);assert(sockets.size()==count);}
#else
 {uint8_t jpeg[StreamClient::jpeg_capacity]{};StreamClient s;init(s,jpeg);auto* p=connect(s,11000);auto f=message(1,1,20);assert(receive(p,f.data(),f.size())==ERR_OK);ReceivedFrame got;assert(s.take_frame(got));assert(!s.prefetch_credit(got)&&p->writes.size()==8);assert(s.frame_done(got)&&p->writes.size()==9);s.close();}
#endif

  std::puts("native StreamClient mock: header splits/chained pbufs/status-during-decode/credit/cap/reconnect/abort/setup/output retry passed");
}
'''
}
def main():
    compiler = os.environ.get("HOST_CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler or not shutil.which(compiler):
        raise SystemExit("a host C++ compiler is required")
    with tempfile.TemporaryDirectory(prefix='streamer-native-test-') as directory:
        base = Path(directory)
        for name, content in files.items():
            p=base/name;p.parent.mkdir(parents=True, exist_ok=True);p.write_text(content)
        command=[compiler,'-std=c++17','-DNATIVE_TRACE_GAPS=1','-DNATIVE_TRACE_TCP_INGRESS=1','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined', '-I'+str(base), '-I'+str(root/'native'), '-I'+str(root/'esp/components/protocol/include'), str(root/'native/stream_client.cpp'),str(base/'review.cpp'),'-o',str(base/'review')]
        for psram in (0, 1):
            subprocess.run(command + [f'-DNATIVE_JPEG_PSRAM={psram}'], check=True, timeout=60)
            subprocess.run([str(base/'review')], check=True, timeout=30)
            print(f'PSRAM input profile {psram}: passed')


if __name__ == "__main__":
    main()
