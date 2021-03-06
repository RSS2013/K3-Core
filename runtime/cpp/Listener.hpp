#ifndef K3_RUNTIME_LISTENER_H
#define K3_RUNTIME_LISTENER_H

#include <atomic>
#include <memory>
#include <unordered_set>
#include <boost/array.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/locks.hpp> 
#include <boost/thread/lock_types.hpp> 
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <k3/runtime/cpp/Common.hpp>
#include <k3/runtime/cpp/Queue.hpp>
#include <k3/runtime/cpp/IOHandle.hpp>
#include <k3/runtime/cpp/Endpoint.hpp>

namespace K3 {
  
  using namespace std;
  using namespace boost;

  using std::atomic_uint;

  using boost::condition_variable;
  using boost::lock_guard;
  using boost::mutex;
  using boost::unique_lock;

  //--------------------------------------------
  // A reference counter for listener instances.

  class ListenerCounter : public atomic_uint {
  public:
    ListenerCounter() : atomic_uint(0) {}

    void registerListener() { this->fetch_add(1); }
    void deregisterListener() { this->fetch_sub(1); }
    unsigned int operator()() { return this->load(); }
  };

  //---------------------------------------------------------------
  // Control data structures for multi-threaded listener execution.

  class ListenerControl {
  public:

    ListenerControl(shared_ptr<mutex> m, shared_ptr<condition_variable> c,
                    shared_ptr<ListenerCounter> i)
      : listenerCounter(i), msgAvailable(false), msgAvailMutex(m), msgAvailCondition(c)
    {}

    // Waits on the message available condition variable.
    void waitForMessage()
    {
      unique_lock<mutex> lock(*msgAvailMutex);
      while ( !msgAvailable ) { msgAvailCondition->wait(lock); }
    }

    // Notifies one waiter on the message available condition variable.
    void messageAvailable()
    {
      {
        lock_guard<mutex> lock(*msgAvailMutex);
        msgAvailable = true;
      }
      msgAvailCondition->notify_one();
    }

    shared_ptr<ListenerCounter> counter() { return listenerCounter; }

    shared_ptr<mutex> msgMutex() { return msgAvailMutex; }
    shared_ptr<condition_variable> msgCondition() { return msgAvailCondition; }

  protected:
    shared_ptr<ListenerCounter> listenerCounter;

    bool msgAvailable;
    shared_ptr<mutex> msgAvailMutex;
    shared_ptr<condition_variable> msgAvailCondition;
  };

  //-------------------------------
  // Listener processor base class.

  template<typename Value, typename EventValue>
  class ListenerProcessor : public virtual LogMT {
  public:
    ListenerProcessor(shared_ptr<ListenerControl> c, shared_ptr<Endpoint<Value, EventValue> > e)
      : LogMT("ListenerProcessor"), control(c), endpoint(e)
    {}

    // A callable processing function that must be implemented by subclasses.
    virtual void operator()() = 0;
  
  protected:
    shared_ptr<ListenerControl> control;
    shared_ptr<Endpoint<Value, EventValue> > endpoint;
  };

  //------------------------------------
  // Listener processor implementations.

  template<typename Value, typename EventValue>
  class InternalListenerProcessor : public ListenerProcessor<Message<Value>, EventValue>
  {
  public:
    InternalListenerProcessor(shared_ptr<MessageQueues<Value> > q,
                              shared_ptr<ListenerControl> c,
                              shared_ptr<Endpoint<Value, EventValue> > e)
      : ListenerProcessor<Message<Value>, EventValue>(c, e), engineQueues(q)
    {}

    void operator()() {
      if ( this->control && this->endpoint && engineQueues )
      {
        // Enqueue from the endpoint's buffer into the engine queues,
        // and signal message availability.
        this->endpoint->buffer()->enqueue(engineQueues);
        this->control->messageAvailable();
      }
      else { 
        this->logAt(boost::log::trivial::error, "Invalid listener processor members");
      }
    }

  protected:
    shared_ptr<MessageQueues<Value> > engineQueues;
  };
  
  template<typename Value, typename EventValue>
  class ExternalListenerProcessor : public ListenerProcessor<Value, EventValue>
  {
  public:
    ExternalListenerProcessor(shared_ptr<ListenerControl> c,
                              shared_ptr<Endpoint<Value, EventValue> > e)
      : ListenerProcessor<Value, EventValue>(c, e)
    {}

    void operator()() {
      if ( this->control && this->endpoint )
      {
        // Notify the endpoint's subscribers of a socket data event,
        // and signal message availability.
        this->endpoint->subscribers()->notifyEvent(EndpointNotification::SocketData);
        this->control->messageAvailable();
      }
      else { 
        this->logAt(boost::log::trivial::error, "Invalid listener processor members");
      }
    }
  };

  //------------
  // Listeners

  // Abstract base class for listeners.
  template<typename Value, typename EventValue, typename NContext, typename NEndpoint>
  class Listener : public virtual LogMT
  {
  public:
    Listener(Identifier n,
             shared_ptr<NContext> ctxt,
             shared_ptr<Endpoint<Value, EventValue> > ep,
             shared_ptr<ListenerControl> ctrl,
             shared_ptr<ListenerProcessor<Value, EventValue> > p)
      : LogMT("Listener_"+n), name(n), ctxt_(ctxt), endpoint_(ep), control_(ctrl), processor_(p)
    {
      if ( endpoint_ ) {
        typename IOHandle<Value>::SourceDetails source = ep->handle()->networkSource();
        nEndpoint_ = get<0>(source);
        wireDesc_ = get<1>(source);
      }
    }

  protected:
    Identifier name;
    
    shared_ptr<NContext> ctxt_;
    shared_ptr<Endpoint<Value, EventValue> > endpoint_;
    shared_ptr<NEndpoint> nEndpoint_;
    shared_ptr<WireDesc<Value> > wireDesc_;
      // We assume this wire description performs the framing necessary
      // for partial network messages.
    
    shared_ptr<ListenerControl> control_;
    shared_ptr<ListenerProcessor<Value, EventValue> > processor_;
  };

  namespace Asio
  {
    using namespace boost::asio;
    using namespace boost::log;
    using namespace boost::system;

    using boost::system::error_code;

    template<typename Value, typename EventValue, typename NContext, typename NEndpoint>
    using BaseListener = ::K3::Listener<Value, EventValue, NContext, NEndpoint>;

    // TODO: close method, terminating all incoming connections to this acceptor.
    template<typename Value, typename EventValue>
    class Listener : public BaseListener<Value, EventValue, NContext, NEndpoint>,
                     public basic_lockable_adapter<mutex>
    {
    public:
      typedef basic_lockable_adapter<mutex> llockable;
      typedef list<shared_ptr<NConnection> > ConnectionList;
      typedef externally_locked<shared_ptr<ConnectionList>, Listener> ConcurrentConnectionList;

      Listener(Identifier n,
               shared_ptr<NContext> ctxt,
               shared_ptr<Endpoint<Value, EventValue> > ep,
               shared_ptr<ListenerControl> ctrl,
               shared_ptr<ListenerProcessor<Value, EventValue> > p)
        : BaseListener<Value, EventValue, NContext, NEndpoint>(n, ctxt, ep, ctrl, p),
          llockable(), connections_(emptyConnections())
      {
        if ( this->nEndpoint_ && this->wireDesc_
                && this->ctxt_ && this->ctxt_->service_threads )
        {
          acceptConnection();
          thread_ = shared_ptr<thread>(this->ctxt_->service_threads->create_thread(*(this->ctxt_)));
        } else {
          this->logAt(trivial::error, "Invalid listener arguments.");
        }
      }

    protected:
      shared_ptr<thread> thread_;
      shared_ptr<externally_locked<shared_ptr<ConnectionList>, Listener> > connections_;

      //---------
      // Helpers.

      shared_ptr<ConcurrentConnectionList> emptyConnections()
      {
        shared_ptr<ConnectionList> l = shared_ptr<ConnectionList>(new ConnectionList());
        return shared_ptr<ConcurrentConnectionList>(new ConcurrentConnectionList(*this, l));
      }

      //---------------------
      // Endpoint execution.

      void acceptConnection()
      {
        if ( this->endpoint_ && this->wireDesc_ ) {
          shared_ptr<NConnection> nextConnection = shared_ptr<NConnection>(new NConnection(this->ctxt_));
          
          this->nEndpoint_->acceptor()->async_accept(nextConnection,
            [=] (const error_code& ec) {
              if ( !ec ) { registerConnection(nextConnection); }
              else { this->logAt(trivial::error, string("Failed to accept a connection: ")+ec.message()); }
            });

          acceptConnection();
        }
        else { this->logAt(trivial::error, "Invalid listener endpoint or wire description"); }
      }

      void registerConnection(shared_ptr<NConnection> c)
      {
        if ( connections_ ) {
          {
            strict_lock<Listener> guard(*this);
            connections_->get(guard)->push_back(c);
          }
          
          // Notify subscribers of socket accept event.
          if ( this->endpoint_ ) { 
            this->endpoint_->subscribers()->notifyEvent(EndpointNotification::SocketAccept);
          }
          
          // Start connection.
          receiveMessages(c);
        }
      }

      void deregisterConnection(shared_ptr<NConnection> c)
      {
        if ( connections_ ) {
          strict_lock<Listener> guard(*this);
          connections_->get(guard)->remove(c);
        }
      }

      void receiveMessages(shared_ptr<NConnection> c)
      {
        if ( c && c->socket() && this->processor_ )
        {
          // TODO: extensible buffer size.
          // We use a local variable for the socket buffer since multiple threads
          // may invoke this handler simultaneously (i.e. for different connections).
          typedef boost::array<char, 8192> SocketBuffer;
          shared_ptr<SocketBuffer> buffer_(new SocketBuffer());

          async_read(c->socket(), buffer(buffer_->c_array(), buffer_->size()),
            [=](const error_code& ec, std::size_t bytes_transferred)
            {
              if ( !ec )
              {
                // Unpack buffer, check if it returns a valid message, and pass that to the processor.
                // We assume the processor notifies subscribers regarding socket data events.
                shared_ptr<Value> v = this->wireDesc_->unpack(string(buffer_->c_array(), buffer_->size()));
                if ( v ) { 
                  // Add the value to the endpoint's buffer, and invoke the listener processor.
                  this->endpoint_->buffer()->append(v);
                  (*(this->processor_))();
                }
                
                // Recursive invocation for the next message.
                receiveMessages(c);
              }
              else 
              {
                deregisterConnection(c);
                this->logAt(trivial::error, string("Connection error: ")+ec.message());
              }
            });
        }
        else { this->logAt(trivial::error, "Invalid listener connection"); }
      }
    };
  }

  namespace Nanomsg 
  {
    using std::atomic_bool;

    template<typename Value, typename EventValue, typename NContext, typename NEndpoint>
    using BaseListener = ::K3::Listener<Value, EventValue, NContext, NEndpoint>;

    template<typename Value, typename EventValue>
    class Listener : public BaseListener<Value, EventValue, NContext, NEndpoint>
    {
    public:
      Listener(Identifier n,
               shared_ptr<NContext> ctxt,
               shared_ptr<Endpoint<Value, EventValue> > ep,
               shared_ptr<ListenerControl> ctrl,
               shared_ptr<ListenerProcessor<Value, EventValue> > p)
        : BaseListener<Value, EventValue, NContext, NEndpoint>(n, ctxt, ep, ctrl, p)
      {
        if ( this->nEndpoint_ && this->wireDesc_ && this->ctxt_ && this->ctxt_->listenerThreads ) {
          // Instantiate a new thread to listen for messages on the nanomsg
          // socket, tracking it in the network context.
          terminated_ = false;
          thread_ = shared_ptr<thread>(this->ctxt_->listenerThreads->create_thread(*this));
        } else {
          this->logAt(trivial::error, "Invalid listener arguments.");
        }
      }

      void operator()()
      {
        typedef boost::array<char, 8192> SocketBuffer;
        SocketBuffer buffer_;

        while ( !terminated_ ) {
          // Receive a message.          
          int bytes = nn_recv(this->endpoint_->acceptor(), buffer_.c_array(), buffer_.static_size, 0);
          if ( bytes >= 0 ) {
            // Unpack, process.
            shared_ptr<Value> v = this->wireDesc_->unpack(string(buffer_.c_array(), buffer_.static_size));
            if ( v ) { 
              // Simulate accept events for nanomsg.
              refreshSenders(v);

              // Add the value to the endpoint's buffer, and invoke the listener processor.
              this->endpoint_->buffer()->append(v);
              (*(this->processor_))();
            }
          }
          else {
            this->logAt(trivial::error, string("Error receiving message: ") + nn_strerror(nn_errno()));
            terminate();
          }
        }
      }

      void terminate() { terminated_ = true; }

    protected:
      shared_ptr<thread> thread_;
      atomic_bool terminated_;
      unordered_set<string> senders;

      // TODO: simulate socket accept notifications. Since nanomsg is not connection-oriented,
      // we simulate connections based on the arrival of messages from unseen addresses.
      // TODO: break assumption that value is a Message.
      void refreshSenders(shared_ptr<Value> v) {
        /*
        if ( senders.find(v->address()) == senders.end() ) {
          senders.insert(v->address());
          endpoint_->subscribers()->notifyEvent(EndpointNotification::SocketAccept);
        }

        // TODO: remove addresses from the recipients pool based on a timeout.
        // TODO: time out stale senders.
        */
      }
    };
  }
} 

#endif