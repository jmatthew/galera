#include "check_gcomm.hpp"

#include "gcomm/transport.hpp"
#include "gcomm/thread.hpp"

#include <algorithm>
#include <check.h>


using namespace gcomm;



const char* nonblock_addr = "gcomm+tcp://127.0.0.1:23456?tcp.non_blocking=1";
const char* block_addr = "gcomm+tcp://127.0.0.1:4567";

class Sender : Toplay
{
    Transport *tp;
    EventLoop *el;
    bool can_send;
public:
    Sender(EventLoop *el_) : 
        tp(0), 
        el(el_), 
        can_send(false) 
    {
    }
    void handle_up(const int cid, const ReadBuf *rb, const size_t offset,
                   const ProtoUpMeta *um)
    {
        if (rb == 0 && tp->get_state() == S_CONNECTED)
            can_send = true;
        else
            throw FatalException("");
    }

    bool get_connected() const
    {
        return can_send;
    }
    
    void connect()
    {
        URI uri(nonblock_addr);
        uri.set_query_param("tcp.max_pending", Int(1 << 25).to_string());
        tp = Transport::create(uri, el);
        gcomm::connect(tp, this);
        tp->connect();
    }
    
    bool send(const size_t b)
    {
        unsigned char *buf = new unsigned char[b];
        if (can_send == false)
            throw FatalException("");
        
        for (size_t i = 0; i < b; i++)
            buf[i] = i % 256;
        WriteBuf wb(buf, b);
        int res;
        bool ret = (res = pass_down(&wb, 0)) == 0 ? true : false;
        delete[] buf;
        return ret;
    }

    void send_block(const size_t b)
    {
        unsigned char *buf = new unsigned char[b];
        if (can_send == false)
            throw FatalException("");
        for (size_t i = 0; i < b; i++)
            buf[i] = i % 256;
        WriteBuf wb(buf, b);
        tp->send(&wb, 0);
        delete[] buf;
    }
    
    void close()
    {
        tp->close();
        delete tp;
        tp = 0;
        can_send = false;
    }
};




class Receiver : public Toplay
{
    clock_t cstart;
    clock_t cstop;
    uint64_t recvd;
    Transport *tp;
public:
    Receiver(Transport *t) : recvd(0), tp(t)
    {
        cstart = clock();
        gcomm::connect(tp, this);
    }
    ~Receiver()
    {
        cstop = clock();
        clock_t ct = (cstop - cstart);
        double tput = CLOCKS_PER_SEC*double(recvd)/(cstop - cstart);
        LOG_INFO("Reciver: received " 
                 + UInt64(recvd).to_string() 
                 + " bytes\n"
                 + "         used " 
                 + Double((double(ct)/CLOCKS_PER_SEC)).to_string() 
                 + "secs cputime\n"
                 + "         throughput " 
                 + Double(tput).to_string() 
                 + "bytes/cpusec");
        tp->close();
        delete tp;
    }

    void handle_up(const int cid, const ReadBuf *rb, const size_t offset,
                   const ProtoUpMeta *um)
    {
        const unsigned char *ptr;
        if (rb == 0) {
            if (tp->get_state() == S_FAILED)
                throw FatalException(strerror(tp->get_errno()));
            return;
        }
        for (size_t i = rb->get_len(); i < rb->get_len() - offset;
             i++) {
            ptr = reinterpret_cast<const unsigned char *>(rb->get_buf(i));
            if (*ptr != i % 256)
                throw FatalException("");
        }

        recvd += rb->get_len() - offset;
    }

    void recv()
    {
        const ReadBuf *rb = tp->recv();
        if (rb == 0)
            throw FatalException("");
        const unsigned char *ptr;
        for (size_t i = rb->get_len(); i < rb->get_len(); i++) {
            ptr = reinterpret_cast<const unsigned char *>(rb->get_buf(i));
            if (*ptr != i % 256)
                throw FatalException("");
        }
    }
};

void release(std::pair<Receiver *, Transport *>& p)
{
    delete p.first;
}

class Listener : public Toplay
{
    Transport* tp;
    EventLoop* el;
    std::list<std::pair<Receiver *, Transport *> > tports;
public:
    Receiver *get_first_receiver() {
        return tports.front().first;
    }
    Listener(EventLoop* el_) : 
        tp(0), 
        el(el_)
    {
    }
    
    ~Listener()
    {
        if (tp)
        {
            stop();
        }
    }

    void handle_up(const int cid, const ReadBuf *rb, const size_t offset,
                   const ProtoUpMeta *um)
    {
        Transport *t = tp->accept();
        Receiver *r = new Receiver(t);
        tports.push_back(std::pair<Receiver *, Transport *>(r, t));
    }
    
    int handle_down(WriteBuf *wb, const ProtoDownMeta *dm)
    {
        throw FatalException("");
        return 0;
    }

    void start()
    {
        if (tp)
            throw FatalException("");
        URI uri(nonblock_addr);
        uri.set_query_param("tcp.max_pending", Int(1 << 25).to_string());
        tp = Transport::create(uri, el);
        gcomm::connect(tp, this);
        tp->listen();
    }
    
    void stop()
    {
        tp->close();
        delete tp;
        tp = 0;
        for_each(tports.begin(), tports.end(), ::release);
        tports.clear();
    }
};

START_TEST(test_nonblock)
{
    EventLoop el;
    Listener l(&el);
    Sender s(&el);
    l.start();
    s.connect();
    el.poll(1);
    
    while (s.get_connected() == false)
    {
        el.poll(1);
    }

    LOG_INFO("connected");
    
    for (size_t i = 1; i <= (1 << 24);)
    {
        LOG_INFO("sending " + Size(i).to_string());
        if (s.send(i) == true)
            i *= 2;
        el.poll(1);
    }
    
    while (el.poll(1) > 0) {}
    
    s.close();
    l.stop();
}
END_TEST

START_TEST(test_nonblock_multi)
{
    EventLoop el;
    EventLoop* el_p = &el;
    Listener l(el_p);
    Sender s1(el_p), s2(el_p), s3(el_p);
    l.start();
    s1.connect();
    s2.connect();
    s3.connect();
    el.poll(1);
    
    while (s1.get_connected() == false ||
           s2.get_connected() == false ||
           s3.get_connected() == false) {
        el.poll(1);
    }
    
    for (int i = 0; i < 10000; i++) {
        s1.send(rand()%10000);
        s2.send(rand()%10000);
        s3.send(rand()%10000);
        el.poll(1);
    }

    while (el.poll(1) > 0) {}

    s1.close();
    s2.close();
    s3.close();
    l.stop();
}
END_TEST


class BlockSender : public Thread
{
    const char* addr;
    Transport* tp;
    size_t sent;
    unsigned char* buf;
public:

    BlockSender(const char* a) : addr(a), tp(0), sent(0), buf(0) {}

    ~BlockSender() {
        delete[] buf;
        LOG_INFO("BlockSender: sent " + Size(sent).to_string() + " bytes");
    }

    void send(size_t len) {
        buf = new unsigned char[len];
        for (size_t i = 0; i < len; i++)
            buf[i] = i % 256;
        WriteBuf wb(buf, len);
        tp->send(&wb, 0);
        delete[] buf;
        buf = 0;
        sent += len;
        // std::cerr << "sent " << len;
    }

    void run() {
        for (int i = 0; i < 40000; i++) {
            send(::rand()%1024);
        }
    }

    void start()
    {
        tp = Transport::create(URI(addr), 0);
        tp->connect();
        Thread::start();
    }
    
    void stop()
    {
        Thread::stop();
        tp->close();
        delete tp;
    }

};

class BlockReceiver : public Thread {
    Transport* tp;
    size_t recvd;
public:
    BlockReceiver(Transport* t) : 
        tp(t), 
        recvd(0)
    {
    }
    
    ~BlockReceiver()
    {
        LOG_INFO("BlockReceiver: received " + Size(recvd).to_string() + " bytes");
        tp->close();
        delete tp;
    }
    
    void run()
    {
        const ReadBuf* rb;
        while ((rb = tp->recv()) != 0) {
            size_t len = rb->get_len();
            const unsigned char *buf = reinterpret_cast<const unsigned char *>(rb->get_buf());
            for (size_t i = 0; i < len; i++)
                if (buf[i] != i % 256)
                    fail_unless(buf[i] == i % 256);
            recvd += len;
            // std::cerr << " recv " << len;
        }
    }
};

class BlockListener : public Thread
{
    Transport *listener;
    const char *addr;
    std::list<BlockReceiver*> recvrs;
public:
    BlockListener(const char *a) : addr(a)
    {
    }
    
    
    void run() {
        Transport* tp;
        while ((tp = listener->accept()))
        {
            BlockReceiver* r = new BlockReceiver(tp);
            recvrs.push_back(r);
            r->start();
        }
    }

    void start()
    {
        listener = Transport::create(URI(addr), 0);
        listener->listen();
        Thread::start();
    }
    
    void stop()
    {
        Thread::stop();
        listener->close();
        delete listener;
        
        for (std::list<BlockReceiver*>::iterator i = recvrs.begin();
             i != recvrs.end(); ++i) {
            (*i)->stop();
            delete *i;
        }
    }
};

START_TEST(test_block)
{
    BlockListener l(block_addr);
    l.start();

    BlockSender s(block_addr);
    s.start();
    ::sleep(2);

    s.stop();
    l.stop();

}
END_TEST


Suite* tcp_suite()
{
    Suite* s = suite_create("tcp");
    TCase* tc;

    tc = tcase_create("test_nonblock");
    tcase_add_test(tc, test_nonblock);
    suite_add_tcase(s, tc);

    tc = tcase_create("test_nonblock_multi");
    tcase_add_test(tc, test_nonblock_multi);
    tcase_set_timeout(tc, 10);
    suite_add_tcase(s, tc);


    tc = tcase_create("test_block");
    tcase_add_test(tc, test_block);
    suite_add_tcase(s, tc);

    return s;
}
