#include <thrift/server/TServer.h>
#include <thrift/protocol/TProtocol.h>
#include <thrift/transport/TVirtualTransport.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TTransportUtils.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>
#include "gen-cpp/Echo.h"
#include "spsc_queue.h"
#include <thrift/server/TSimpleServer.h>

using apache::thrift::TProcessor;
using apache::thrift::TProcessorFactory;
using apache::thrift::protocol::TBinaryProtocol;
using apache::thrift::protocol::TProtocol;
using apache::thrift::protocol::TProtocolFactory;
using apache::thrift::server::TServer;
using apache::thrift::transport::TFramedTransport;
using apache::thrift::transport::TMemoryBuffer;
using apache::thrift::transport::TNullTransport;
using apache::thrift::transport::TTransport;
using apache::thrift::transport::TTransportException;
using apache::thrift::transport::TTransportFactory;
using apache::thrift::transport::TVirtualTransport;
using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;
using namespace echo;

struct SHMData
{
    char data[4096][4096];
};

class SHMServer : public TServer
{
public:
    template <typename ProcessorFactory>
    SHMServer(const boost::shared_ptr<ProcessorFactory> &processorFactory,
              const std::string &shm_path, int64_t size) : TServer(processorFactory),
                                                           shm_path_(shm_path), size_(size)
    {
        nullTransport_.reset(new TNullTransport());
        inputTransport_.reset(new TMemoryBuffer(NULL, 0));
        outputTransport_.reset(new TMemoryBuffer());
        factoryInputTransport_ = getInputTransportFactory()->getTransport(inputTransport_);
        factoryOutputTransport_ = getOutputTransportFactory()->getTransport(outputTransport_);

        inputProtocol_ = getInputProtocolFactory()->getProtocol(factoryInputTransport_);
        outputProtocol_ = getOutputProtocolFactory()->getProtocol(factoryOutputTransport_);

        processor_ = getProcessor(inputProtocol_, outputProtocol_, nullTransport_);
    }
    void serve()
    {

        start();
    }

    void start()
    {
        q_ = spsc_var_queue_init_shm<SHMData>(shm_path_.c_str(), size_);
        while (true)
        {
            spsc_var_queue_block *msg = (spsc_var_queue_block *)spsc_var_queue_read(q_);
            if (msg == nullptr)
                continue;
            int64_t size = (msg - 1)->size - sizeof(spsc_var_queue_block);
            int seqid = *((int *)msg);
            char *data = (char *)msg + sizeof(int);
            inputTransport_->resetBuffer((uint8_t *)data, size, TMemoryBuffer::COPY);
            outputTransport_->resetBuffer();
            process(seqid);
            spsc_var_queue_pop(q_);
        }
    }
    void process(int seqid)
    {
        try
        {
            processor_->process(inputProtocol_, outputProtocol_, NULL);
            uint8_t *buf;
            uint32_t size;
            outputTransport_->getBuffer(&buf, &size);
            char *output = &q_->udata.data[seqid][2];
            memcpy(output, buf, size);
            q_->udata.data[seqid][1] = 1;
        }
        catch (const TTransportException &ex)
        {
            std::cout << "ThriftServer TTransportException: " << ex.what() << "\n";
        }
        catch (const std::exception &ex)
        {
            std::cout << "ThriftServer std::exception: " << ex.what() << "\n";
        }
        catch (...)
        {
            std::cout << "ThriftServer unknown exception"
                      << "\n";
        }
    }

private:
    int64_t size_;
    std::string shm_path_;
    spsc_var_queue<SHMData> *q_;
    boost::shared_ptr<TNullTransport> nullTransport_;

    boost::shared_ptr<TMemoryBuffer> inputTransport_;
    boost::shared_ptr<TMemoryBuffer> outputTransport_;

    boost::shared_ptr<TTransport> factoryInputTransport_;
    boost::shared_ptr<TTransport> factoryOutputTransport_;

    boost::shared_ptr<TProtocol> inputProtocol_;
    boost::shared_ptr<TProtocol> outputProtocol_;

    boost::shared_ptr<TProcessor> processor_;
};

class SHMTransport : public TVirtualTransport<SHMTransport>
{
public:
    SHMTransport(const std::string &path)
    {
        q_ = spsc_var_queue_connect_shm<SHMData>(path.c_str());
        buffer_ = new char[1024];
        woffset_ = 0;
        roffset_ = 0;
        free_index_ = -1;
    }
    virtual ~SHMTransport() {}

    void open() override {}

    bool isOpen() const { return true; }

    bool peek()
    {
        return true;
    }

    void close() override {}

    uint32_t read(uint8_t *buf, uint32_t len)
    {
        while (q_->udata.data[free_index_][1] == 0)
        {
        }
        char *data = &q_->udata.data[free_index_][2] + roffset_;
        memcpy(buf, data, len);
        roffset_ += len;
        return len;
    }
    uint32_t readEnd()
    {
        spsc_var_queue_spin_lock(q_);
        q_->udata.data[free_index_][0] = 0;
        spsc_var_queue_spin_unlock(q_);
        roffset_ = 0;
        return 0;
    }

    void write(const uint8_t *buf, uint32_t len)
    {
        memcpy(buffer_ + woffset_, buf, len);
        woffset_ += len;
    }
    uint32_t writeEnd()
    {
        return 0;
    }

    void flush()
    {
        spsc_var_queue_spin_lock(q_);
        free_index_ = get_free_index();
        q_->udata.data[free_index_][0] = 1; // lock
        q_->udata.data[free_index_][1] = 0; // not finished
        int alloc_size = woffset_ + sizeof(int);
        char *des = (char *)spsc_var_queue_alloc<SHMData>(q_, alloc_size);
        memcpy(des, &free_index_, sizeof(int));
        memcpy(des + sizeof(int), buffer_, woffset_);
        spsc_var_queue_push<SHMData>(q_);
        woffset_ = 0;
        spsc_var_queue_spin_unlock(q_);
    };

    const std::string getOrigin() const
    {
        return "shm";
    }

private:
    int get_free_index()
    {
        int index = -1;
        do
        {
            for (int i = 0; i < sizeof(SHMData::data[0]); i++)
            {
                if (q_->udata.data[i][0] == 0)
                {
                    index = i;
                    break;
                }
            }
        } while (index == -1);
        return index;
    }

private:
    char *buffer_;
    int woffset_;
    int roffset_;
    int free_index_;
    spsc_var_queue<SHMData> *q_;
};
class EchoHandler : virtual public EchoIf
{
public:
    EchoHandler()
    {
    }

    void echo(std::string &_return, const std::string &arg)
    {
        _return = arg;
        std::cout << "eeeeee\t" << arg << "\n";
    }
    void test(const int32_t time)
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        std::cout << "echo ==========>" << tv.tv_usec - time << "\n";
    }
};

int main(int argc, char **argv)
{
    if (argv[1][0] == 's')
    {
        boost::shared_ptr<EchoHandler> handler(new EchoHandler());
        boost::shared_ptr<TProcessor> processor(new EchoProcessor(handler));
        SHMServer s(processor, "/test", 1024);
        s.serve();
    }
    if (argv[1][0] == 'x')
    {
        boost::shared_ptr<EchoHandler> handler(new EchoHandler());
        boost::shared_ptr<TProcessor> processor(new EchoProcessor(handler));
        boost::shared_ptr<TServerTransport> serverTransport(new TServerSocket(9090));
        boost::shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());
        boost::shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());
        TSimpleServer server(processor, serverTransport, transportFactory, protocolFactory);
        server.serve();
    }
    if (argv[1][0] == 'b')
    {
        boost::shared_ptr<TTransport> socket(new TSocket("127.0.0.1", 9090));
        boost::shared_ptr<TProtocol> protocol(new TBinaryProtocol(socket));
        EchoClient *client = new EchoClient(protocol);
        socket->open();
        struct timeval tv;
        while (true)
        {
            sleep(1);
            gettimeofday(&tv, NULL);
            client->test(tv.tv_usec);
        }
    }
    if (argv[1][0] == 'c')
    {
        boost::shared_ptr<TTransport> socket(new SHMTransport("/test"));
        boost::shared_ptr<TProtocol> protocol(new TBinaryProtocol(socket));
        EchoClient *client = new EchoClient(protocol);
        socket->open();
        struct timeval tv, tv2;
        while (true)
        {
            // sleep(1);
            gettimeofday(&tv, NULL);
            std::string _arg;
            client->echo(_arg, "test");
            gettimeofday(&tv2, NULL);
            std::cout << "echo  " << _arg << "\t" << tv2.tv_usec - tv.tv_usec << "\n";
            // client->test(tv.tv_usec);
        }
    }
}
