/**
 * @file rpc.h
 *
 * Remote procedure call support for FASTlib.  Yay!
 */

#ifndef NBR_RPC_H
#define NBR_RPC_H

#include "blockdev.h"

#include "fastlib/fastlib_int.h"

#include "rpc_sock.h"

/**
 * A single remote procedure call transaction.
 *
 * This automatically handles all the memory management that is involved
 * with marshalling and unmarshalling, freeing memory when the Rpc object
 * is destructed.
 */
template<class ResponseObject>
class Rpc {
  FORBID_COPY(Rpc);
 private:
  template<class RequestObject>
  struct RpcRequestTransaction : public Transaction {
    FORBID_COPY(RpcRequestTransaction);

   public:
    Message* response;
    Mutex mutex;
    WaitCondition cond;

   public:
    RpcRequestTransaction() {}
    virtual ~RpcRequestTransaction() {}

    Message *Doit(int channel, int peer, const RequestObject& request) {
      Transaction::Init(channel);
      Message *message = CreateMessage(peer, ot::PointerFrozenSize(request));
      ot::PointerFreeze(request, message->data());
      response = NULL;
      Send(message);
      mutex.Lock();
      while (response == NULL) {
        cond.Wait(&mutex);
      }
      mutex.Unlock();
      return response;
    }

    void HandleMessage(Message *message) {
      Done();
      mutex.Lock();
      response = message;
      cond.Signal();
      mutex.Unlock();
      // TODO: Handle done
    }
  };
  
 private:
  Message *response_;
  ResponseObject *response_object_;

 public:
  template<typename RequestObject>
  Rpc(int channel, int peer, const RequestObject& request) {
    Request(channel, peer, request);
  }
  Rpc() {
  }
  ~Rpc() {
    if (response_ != NULL) {
      delete response_;
    }
  }

  template<typename RequestObject>
  ResponseObject *Request(
      int channel, int peer, const RequestObject& request) {
    RpcRequestTransaction<RequestObject> transaction;
    response_ = transaction.Doit(channel, peer, request);
    response_object_ = ot::PointerThaw<ResponseObject>(response_->data());
    return response_object_;
  }

  operator ResponseObject *() {
    return response_object_;
  }
  ResponseObject* operator ->() {
    return response_object_;
  }
  ResponseObject& operator *() {
    return *response_object_;
  }
  operator const ResponseObject *() const {
    return response_object_;
  }
  const ResponseObject* operator ->() const {
    return response_object_;
  }
  const ResponseObject& operator *() const {
    return *response_object_;
  }
};

/**
 * This is how you define the network object on the server.
 */
template<typename RequestObject, typename ResponseObject>
class RemoteObjectBackend : public Channel { 
  FORBID_COPY(RemoteObjectBackend);

 public:
  // Simple request-response transaction
  class RemoteObjectTransaction : public Transaction {
    FORBID_COPY(RemoteObjectTransaction);

   private:
    RemoteObjectBackend *inner_;

   public:
    RemoteObjectTransaction() {}
    virtual ~RemoteObjectTransaction() {}
    
    void Init(int channel_num, RemoteObjectBackend *inner_in) { 
      Transaction::Init(channel_num);
      inner_ = inner_in;
    }

    virtual void HandleMessage(Message *request);
  };

 public:
  RemoteObjectBackend() {}
  virtual ~RemoteObjectBackend() {}

  virtual void HandleRequest(const RequestObject& request,
      ResponseObject *response) {
    FATAL("Virtuality sucks");
  }

  virtual Transaction *GetTransaction(Message *message) {
    RemoteObjectTransaction *t = new RemoteObjectTransaction();
    t->Init(message->channel(), this);
    return t;
  }

  void Register(int channel_num) {
    rpc::Register(channel_num, this);
  }
};

template<typename RequestObject, typename ResponseObject>
void RemoteObjectBackend<RequestObject, ResponseObject>
    ::RemoteObjectTransaction::HandleMessage(Message *request) {
  const RequestObject* real_request =
      ot::PointerThaw<RequestObject>(request->data());
  ResponseObject real_response;
  inner_->HandleRequest(*real_request, &real_response);
  Message *response = CreateMessage(
      request->peer(), ot::PointerFrozenSize(real_response));
  delete request;
  ot::PointerFreeze(real_response, response->data());
  Send(response);
  Done();
  delete this;
}

//--------------------------------------------------------------------------

template<typename TReductor, typename TData>
class ReduceChannel : public Channel {
  FORBID_COPY(ReduceChannel);

 private:
  class ReduceTransaction : public Transaction {
    FORBID_COPY(ReduceTransaction);

   private:
    ArrayList<Message*> received_;
    int n_received_;
    TData *data_;
    const TReductor *reductor_;
    DoneCondition cond_;

   private:
    void CheckStatus_() {
      if (n_received_ == rpc::n_children()) {
        for (index_t i = 0; i < rpc::n_children(); i++) {
          TData *subdata = ot::PointerThaw<TData>(received_[i]->data());
          reductor_->Reduce(*subdata, data_);
          delete received_[i];
        }
        if (!rpc::is_root()) {
          // Send my subtree's results to my parent.
          Message *message_to_send = CreateMessage(rpc::parent(),
              ot::PointerFrozenSize(*data_));
          ot::PointerFreeze(*data_, message_to_send->data());
          Send(message_to_send);
        }
        rpc::Unregister(channel());
        Done();
        cond_.Done();
      }
    }

   public:
    ReduceTransaction() {}
    virtual ~ReduceTransaction() {}

    void Init(int channel_num, const TReductor *reductor_in, TData *data_inout) {
      Transaction::Init(channel_num);
      reductor_ = reductor_in;
      received_.Init(rpc::n_children());
      for (index_t i = 0; i < received_.size(); i++) {
        received_[i] = NULL;
      }
      n_received_ = 0;
      data_ = data_inout;
      CheckStatus_();
    }

    void Wait() {
      cond_.Wait();
    }

    void HandleMessage(Message *message) { 
      index_t i;
      for (i = rpc::n_children(); i--;) {
        if (message->peer() == rpc::child(i)) {
          break;
        }
      }
      if (unlikely(i < 0)) {
        FATAL("Message from peer #%d unexpected during reduce #%d",
            message->peer(), channel());
      }
      if (received_[i] != NULL) {
        FATAL("Multiple messages from peer #%d during reduce #%d: %p %ld %p %ld %d %d",
            message->peer(), channel(),
            received_[i], long(received_[i]->data_size()),
            message, long(message->data_size()),
            message->channel(), received_[i]->channel());
      }
      received_[i] = message;
      Done(message->peer());

      n_received_++;

      CheckStatus_();
    }
  };

 private:
  ReduceTransaction transaction_;

 public:
  ReduceChannel() {}
  ~ReduceChannel() {}

  void Init(int channel_num, const TReductor *reductor, TData *data) {
    transaction_.Init(channel_num, reductor, data);
    rpc::Register(channel_num, this);
  }
  
  void Wait() {
    transaction_.Wait();
  }

  void Doit(int channel_num, const TReductor& reductor, TData *data) {
    Init(channel_num, &reductor, data);
    Wait();
  }

  Transaction *GetTransaction(Message *message) {
    return &transaction_;
  }
};

//--------------------------------------------------------------------------

struct DataGetterRequest {
  enum Operation { GET_DATA } operation;
  
  OT_DEF(DataGetterRequest) {
    OT_MY_OBJECT(operation);
  }
};

template<typename T>
class DataGetterBackend
    : public RemoteObjectBackend<DataGetterRequest, T> {
 private:
  const T* data_;

 public:
  void Init(const T* data_in) {
    data_ = data_in;
  }

  virtual void HandleRequest(const DataGetterRequest& request, T *response);
};

template<typename T>
void DataGetterBackend<T>::HandleRequest(const DataGetterRequest& request,
    T* response) {
  response->Copy(*data_);
}

namespace rpc {
  template<typename T>
  void GetRemoteData(int channel, int peer, T* result) {
    DataGetterRequest request;
    request.operation = DataGetterRequest::GET_DATA;
    Rpc<T> response(channel, peer, request);
    result->Copy(*response);
  }

  void Barrier(int channel_num);

  /**
   * Performs an efficient distributed recution.
   *
   * The value will be the value for all the machines if rpc::is_root().
   * On entry, provide my sole contribution to the recution.
   * If I am not the root, it will contain the value for all direct and
   * indirect children of this node.
   * The operator is assumed to be associative, but not necessarily
   * commutative, and is processed precisely in the order of the computers.
   *
   * The reductor should have a public method:
   *
   * <code>Reduce(const TData& right_hand, TData* left_hand_to_modify) const</code>
   *
   * The right-hand-side is passed first because it is not modified, but the
   * left-hand-side is being modified.
   *
   * @param channel_num a unique channel number associated with this
   * @param reductor the reductor object
   * @param value on entry, my part of the contribution; on output, the
   *        reduced value for the subtree of processes rooted at the current
   */
  template<typename TReductor, typename TData>
  void Reduce(int channel_num, const TReductor& reductor, TData *value) {
    ReduceChannel<TReductor, TData> channel;
    channel.Doit(channel_num, reductor, value);
  }
};

#endif
