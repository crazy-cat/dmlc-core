/*!
 *  Copyright (c) 2015 by Contributors
 * \file threadediter.h
 * \brief thread backed iterator that can be used to implement
 *   general thread-based pipeline such as prefetch and pre-computation
 * To use the functions in this header, C++11 is required
 * \author Tianqi Chen
 */
#ifndef DMLC_THREADEDITER_H_
#define DMLC_THREADEDITER_H_
// defines DMLC_USE_CXX11
#include "./base.h"
// this code depends on c++11
#if DMLC_ENABLE_STD_THREAD
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>
#include "./data.h"
#include "./logging.h"
#include "concurrency.h"

namespace dmlc {
/*!
 * \brief a iterator that was backed by a thread
 *  to pull data eagerly from a single producer into a bounded buffer
 *  the consumer can pull the data at its own rate
 *
 * NOTE: thread concurrency cost time, make sure to store big blob of data in DType
 *
 * Usage example:
 * \code
 * ThreadedIter<DType> iter;
 * iter.Init(&producer);
 * // the following code can be in parallel
 * DType *dptr;
 * while (iter.Next(&dptr)) {
 *   // do something on dptr
 *   // recycle the space
 *   iter.Recycle(&dptr);
 * }
 * \endcode
 * \tparam DType the type of data blob we support
 */
template<typename DType>
class ThreadedIter : public DataIter<DType> {
 public:
  /*!
   * \brief producer class interface
   *  that threaditer used as source to
   *  preduce the content
   */
  class Producer {
   public:
    // virtual destructor
    virtual ~Producer() {}
    /*! \brief reset the producer to beginning */
    virtual void BeforeFirst(void) {
      NotImplemented();
    }
    /*!
     * \brief load the data content into DType,
     * the caller can pass in NULL or an existing address
     * when inout_dptr is NULL:
     *    producer need to allocate a DType and fill the content
     * when inout_dptr is specified
     *    producer takes need to fill the content into address
     *    specified inout_dptr, or delete the one and create a new one
     *
     * \param inout_dptr used to pass in the data holder cell
     *        and return the address of the cell filled
     * \return true if there is next record, false if we reach the end
     */
    virtual bool Next(DType **inout_dptr) = 0;
  };
  /*!
   * \brief constructor
   * \param max_capacity maximum capacity of the queue
   */
  explicit ThreadedIter(size_t max_capacity = 8)
      : producer_owned_(NULL),
        producer_thread_(NULL),
        max_capacity_(max_capacity),
        nwait_consumer_(0),
        nwait_producer_(0),
        out_data_(NULL) {}
  /*! \brief destructor */
  virtual ~ThreadedIter(void) {
    this->Destroy();
  }
  /*!
   * \brief destroy all the related resources
   *  this is equivalent to destructor, can be used
   *  to destroy the threaditer when user think it is
   *  appropriate, it is safe to call this multiple times
   */
  inline void Destroy(void);
  /*!
   * \brief set maximum capacity of the queue
   * \param max_capacity maximum capacity of the queue
   */
  inline void set_max_capacity(size_t max_capacity) {
    max_capacity_ = max_capacity;
  }
  /*!
   * \brief initialize the producer and start the thread
   *   can only be called once
   * \param producer pointer to the producer
   * \param pass_ownership whether pass the ownership to the iter
   *    if this is true, the threaditer will delete the producer
   *    when destructed
   */
  inline void Init(Producer *producer, bool pass_ownership = false);
  /*!
   * \brief initialize the producer and start the thread
   *  pass in two function(closure) of producer to represent the producer
   *  the beforefirst function is optional, and defaults to not implemented
   *   NOTE: the closure must remain valid until the ThreadedIter destructs
   * \param next the function called to get next element, see Producer.Next
   * \param beforefirst the function to call to reset the producer, see Producer.BeforeFirst
   */
  inline void Init(std::function<bool(DType **)> next,
                   std::function<void()> beforefirst = NotImplemented);
  /*!
   * \brief get the next data, this function is threadsafe
   * \param out_dptr used to hold the pointer to the record
   *  after the function call, the caller takes ownership of the pointer
   *  the caller can call recycle to return ownership back to the threaditer
   *  so that the pointer can be re-used
   * \return true if there is next record, false if we reach the end
   * \sa Recycle
   */
  inline bool Next(DType **out_dptr);
  /*!
   * \brief recycle the data cell, this function is threadsafe
   * the threaditer can reuse the data cell for future data loading
   * \param inout_dptr pointer to the dptr to recycle, after the function call
   *        the content of inout_dptr will be set to NULL
   */
  inline void Recycle(DType **inout_dptr);
  /*!
   * \brief adapt the iterator interface's Next
   *  NOTE: the call to this function is not threadsafe
   *  use the other Next instead
   * \return true if there is next record, false if we reach the end
   */
  virtual bool Next(void) {
    if (out_data_ != NULL) {
      this->Recycle(&out_data_);
    }
    if (Next(&out_data_)) {
      return true;
    } else {
      return false;
    }
  }
  /*!
   * \brief adapt the iterator interface's Value
   *  NOTE: the call to this function is not threadsafe
   *  use the other Next instead
   */
  virtual const DType &Value(void) const {
    CHECK(out_data_ != NULL) << "Calling Value at beginning or end?";
    return *out_data_;
  }
  /*! \brief set the iterator before first location */
  virtual void BeforeFirst(void) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (out_data_ != NULL) {
      free_cells_.push(out_data_);
      out_data_ = NULL;
    }
    if (producer_sig_ == kDestroy)  return;

    producer_sig_ = kBeforeFirst;
    CHECK(!producer_sig_processed_);
    if (nwait_producer_ != 0) {
      producer_cond_.notify_one();
    }
    CHECK(!producer_sig_processed_);
    // wait until the request has been processed
    consumer_cond_.wait(lock, [this]() {
        return producer_sig_processed_;
      });
    producer_sig_processed_ = false;
    bool notify = nwait_producer_ != 0 && !produce_end_;
    lock.unlock();
    // notify producer, in case they are waiting for the condition.
    if (notify) producer_cond_.notify_one();
  }

 private:
  /*! \brief not support BeforeFirst */
  inline static void NotImplemented(void) {
    LOG(FATAL) << "BeforeFirst is not supported";
  }
  /*! \brief signals send to producer */
  enum Signal {
    kProduce,
    kBeforeFirst,
    kDestroy
  };
  /*! \brief producer class */
  Producer *producer_owned_;
  /*! \brief signal to producer */
  Signal producer_sig_;
  /*! \brief whether the special signal other than kProduce is procssed */
  bool producer_sig_processed_;
  /*! \brief thread that runs the producer */
  std::thread *producer_thread_;
  /*! \brief whether produce ends */
  bool produce_end_;
  /*! \brief maximum queue size */
  size_t max_capacity_;
  /*! \brief internal mutex */
  std::mutex mutex_;
  /*! \brief number of consumer waiting */
  unsigned nwait_consumer_;
  /*! \brief number of consumer waiting */
  unsigned nwait_producer_;
  /*! \brief conditional variable for producer thread */
  std::condition_variable producer_cond_;
  /*! \brief conditional variable for consumer threads */
  std::condition_variable consumer_cond_;
  /*! \brief the current output cell */
  DType *out_data_;
  /*! \brief internal queue of producer */
  std::queue<DType*> queue_;
  /*! \brief free cells that can be used */
  std::queue<DType*> free_cells_;
};

// implementation of functions
template<typename DType>
inline void ThreadedIter<DType>::Destroy(void) {
  if (producer_thread_ != NULL) {
    {
      // lock the mutex
      std::lock_guard<std::mutex> lock(mutex_);
      // send destroy signal
      producer_sig_ = kDestroy;
      if (nwait_producer_ != 0) {
        producer_cond_.notify_one();
      }
    }
    producer_thread_->join();
    delete producer_thread_;
    producer_thread_ = NULL;
  }
  // end of critical region
  // now the slave thread should exit
  while (free_cells_.size() != 0) {
    delete free_cells_.front();
    free_cells_.pop();
  }
  while (queue_.size() != 0) {
    delete queue_.front();
    queue_.pop();
  }
  if (producer_owned_ != NULL) {
    delete producer_owned_;
  }
  if (out_data_ != NULL) {
    delete out_data_; out_data_ = NULL;
  }
}

template<typename DType>
inline void ThreadedIter<DType>::
Init(Producer *producer, bool pass_ownership) {
  CHECK(producer_owned_ == NULL) << "can only call Init once";
  if (pass_ownership) producer_owned_ = producer;
  auto next = [producer](DType **dptr) {
      return producer->Next(dptr);
  };
  auto beforefirst = [producer]() {
    producer->BeforeFirst();
  };
  this->Init(next, beforefirst);
}
template<typename DType>
inline void ThreadedIter<DType>::
Init(std::function<bool(DType **)> next,
     std::function<void()> beforefirst) {
  producer_sig_ = kProduce;
  producer_sig_processed_ = false;
  produce_end_ = false;
  // procedure running in prodcuer
  // run producer thread
  auto producer_fun = [this, next, beforefirst] () {
    beforefirst();
    while (true) {
      DType *cell = NULL;
      {
        // lockscope
        std::unique_lock<std::mutex> lock(mutex_);
        ++this->nwait_producer_;
        producer_cond_.wait(lock, [this]() {
            if (producer_sig_ == kProduce) {
              bool ret = !produce_end_ &&
                  (queue_.size() < max_capacity_ || free_cells_.size() != 0);
              return ret;
            } else {
              return true;
            }
          });
        --this->nwait_producer_;
        if (producer_sig_ == kProduce) {
          if (free_cells_.size() != 0) {
            cell = free_cells_.front();
            free_cells_.pop();
          }
        } else if (producer_sig_ == kBeforeFirst) {
          // reset the producer
          beforefirst();
          // cleanup the queue
          while (queue_.size() != 0) {
            free_cells_.push(queue_.front());
            queue_.pop();
          }
          // reset the state
          produce_end_ = false;
          producer_sig_processed_ = true;
          producer_sig_ = kProduce;
          // notify consumer that all the process as been done.
          lock.unlock();
          consumer_cond_.notify_all();
          continue;
        } else {
          // destroy the thread
          CHECK(producer_sig_ == kDestroy);
          producer_sig_processed_ = true;
          produce_end_ = true;
          consumer_cond_.notify_all();
          return;
        }
      }  // end of lock scope
      // now without lock
      produce_end_ = !next(&cell);
      CHECK(cell != NULL || produce_end_);
      bool notify;
      {
        // lockscope
        std::lock_guard<std::mutex> lock(mutex_);
        if (!produce_end_) {
          queue_.push(cell);
        } else {
          if (cell != NULL) free_cells_.push(cell);
        }
        // put things into queue
        notify = nwait_consumer_ != 0;
      }
      if (notify) consumer_cond_.notify_all();
    }
  };
  producer_thread_ = new std::thread(producer_fun);
}

template<typename DType>
inline bool ThreadedIter<DType>::
Next(DType **out_dptr) {
  if (producer_sig_ == kDestroy) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  CHECK(producer_sig_ == kProduce)
      << "Make sure you call BeforeFirst not inconcurrent with Next!";
  ++nwait_consumer_;
  consumer_cond_.wait(lock, [this]() {
      return queue_.size() != 0 || produce_end_;
    });
  --nwait_consumer_;
  if (queue_.size() != 0) {
    *out_dptr = queue_.front();
    queue_.pop();
    bool notify = nwait_producer_ != 0 && !produce_end_;
    lock.unlock();
    if (notify) producer_cond_.notify_one();
    return true;
  } else  {
    CHECK(produce_end_);
    return false;
  }
}

template<typename DType>
inline void ThreadedIter<DType>::Recycle(DType **inout_dptr) {
  bool notify;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    free_cells_.push(*inout_dptr);
    *inout_dptr = NULL;
    notify = nwait_producer_ != 0 && !produce_end_;
  }
  if (notify) producer_cond_.notify_one();
}

/*!
 * \brief ThreadedIter's multi producer version
 *  a iterator that was backed by multi threads
 *  to pull and process data eagerly from multi producer into a bounded buffer
 *  the consumer can pull the data at its own rate
 *
 *  NOTE: the max memory size which MultiThreadedIter internal used is
 *  base.capacity() * SourceType.size +
 *  (queue_capacity_ + thread_num_) * SourceType.size +
 *  (queue_capacity_ + thread_num_) * Dtype.size
 *
 *  vs ThreadedIter: use a ThreadedIter<SourceType> as the origin source
 *  data iterator
 *
 * \tparam DType the type of data blob we support
 * \tparam SourceType the type of source data
 * \tparam base the origin source data iterator
 * \tparam queue_capacity maximum size of the queue
 */
template<typename DType, typename SourceType>
class MultiThreadedIter : public DataIter<DType> {
 public:
  /*!
   * \brief constructor
   * \param base ThreadedIter for source data loading
   * \param thread_num processing thread num
   * \param queue_capacity maximum capacity of the queue
   */
  explicit MultiThreadedIter(ThreadedIter<SourceType>* base,
                             size_t thread_num,
                             size_t queue_capacity) :
      out_data_(nullptr),
      loader_(base),
      thread_num_(thread_num),
      force_stopped_(false),
      null_cell_num_(0),
      queue_(nullptr),
      queue_capacity_(queue_capacity) {
        CHECK(loader_.get() != nullptr);
      }

  /*! \brief destructor */
  virtual ~MultiThreadedIter(void) {
    Destroy();
  }

  /*!
   * \brief destroy all the related resources
   *  this is equivalent to destructor, can be used
   *  to destroy the threaditer when user think it is
   *  appropriate, it is safe to call this multiple times
   */
  inline void Destroy(void);
  /*!
   * \brief initialize the producers and start the threads
   *  pass in two function(closure) of producer to represent the producer
   *   NOTE: the closure must remain valid until the MultiThreadedIter destructs
   * \param next the function called to get next element
   * \param beforefirst the function to call to reset the producer
   */
  inline void Init(std::function<bool(DType**, SourceType*, int)> next,
                   std::function<void()> beforefirst);

  /*!
   * \brief get the next data, this function is not threadsafe
   *  NOTE: this function is not threadsafe
   * \param out_dptr used to hold the pointer to the record
   *  after the function call, the caller takes ownership of the pointer
   *  the caller can call recycle to return ownership back to the threaditer
   *  so that the pointer can be re-used
   * \return true if there is next record, false if we reach the end
   * \sa Recycle
   */
  inline bool Next(DType **out_dptr);

  /*!
   * \brief adapt the iterator interface's Next
   *  NOTE: the call to this function is not threadsafe
   * \return true if there is next record, false if we reach the end
   */
  inline bool Next(void) {
    if (out_data_ != nullptr) {
      Recycle(&out_data_);
    }
    return Next(&out_data_);
  }

  /*!
   * \brief recycle the data cell, this function is threadsafe
   * the threaditer can reuse the data cell for future data loading
   * \param inout_dptr pointer to the dptr to recycle, after the function call
   *        the content of inout_dptr will be set to NULL
   */
  inline void Recycle(DType **inout_dptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_cells_.push(*inout_dptr);
    *inout_dptr = nullptr;
  }

  /*!
   * \brief adapt the iterator interface's Value
   *  NOTE: the call to this function is not threadsafe
   *  use the other Next instead
   */
  virtual const DType &Value(void) const {
    CHECK(out_data_ != nullptr) << "Calling Value at beginning or end?";
    return *out_data_;
  }

  /*! \brief set the iterator before first location */
  virtual void BeforeFirst(void);

 private:
  typedef std::pair<DType*, SourceType*> CellType;

  /*! \brief the current output cell */
  DType* out_data_;
  /*! \brief the source data loader */
  std::unique_ptr<ThreadedIter<SourceType> > loader_;
  /*! \brief internal mutex */
  std::mutex mutex_;
  /*! \brief the producer thread num */
  size_t thread_num_;
  /*! \brief whether force stop producer threads */
  bool force_stopped_;
  /*! \brief null cell num which used to judge source file end */
  size_t null_cell_num_;
  /*! \brief threads that run the producer */
  std::vector<std::unique_ptr<std::thread>> producer_threads_;
  /*! \brief producer thread body */
  std::function<void(int)> producer_thread_body_;
  /*! \brief internal queue of producer */
  std::unique_ptr<ConcurrentBlockingQueue<CellType> > queue_;
  /*! \brief maximum queue size */
  size_t queue_capacity_;
  /*! \brief free cells that can be used */
  std::queue<DType*> free_cells_;
  /*! \brief custom before first function */
  std::function<void()> before_first_;
};

template<typename DType, typename SourceType>
inline void MultiThreadedIter<DType, SourceType>::Init(
    std::function<bool(DType**, SourceType*, int)> next,
    std::function<void()> before_first) {
  queue_.reset(new ConcurrentBlockingQueue<CellType>(queue_capacity_));
  before_first_ = before_first;
  producer_thread_body_ = [this, next] (int tid) {
                            while (true) {
                              SourceType *source_data;
                              if (!loader_->Next(&source_data) || force_stopped_) {
                                queue_->Push(CellType(nullptr, nullptr));
                                return;
                              }
                              // get free cell
                              DType* cell = nullptr;
                              {
                                std::lock_guard<std::mutex> lock(mutex_);
                                if (free_cells_.size() != 0) {
                                  cell = free_cells_.front();
                                  free_cells_.pop();
                                }
                              }
                              // next
                              next(&cell, source_data, tid);
                              queue_->Push(CellType(cell, source_data));
                            }
                          };
  producer_threads_.resize(thread_num_);
  for (size_t tid = 0; tid < thread_num_; tid++) {
    producer_threads_[tid].reset(new std::thread([this, tid]() { producer_thread_body_(tid); }));
  }
}

template<typename DType, typename SourceType>
inline bool MultiThreadedIter<DType, SourceType>::Next(DType **out_dptr) {
  if (null_cell_num_ >= thread_num_) return false;
  CellType cell(nullptr, nullptr);
  while (queue_->Pop(&cell)) {
    if (cell.first != nullptr) {
      *out_dptr = cell.first;
      // recycle source data
      loader_->Recycle(&cell.second);
      return true;
    }
    null_cell_num_++;
    if (null_cell_num_ == thread_num_) {
      return false;
    }
  }
  return false;
}

template<typename DType, typename SourceType>
inline void MultiThreadedIter<DType, SourceType>::Destroy(void) {
  if (force_stopped_) return;
  force_stopped_ = true;
  queue_->SignalForKill();
  for (size_t tid = 0; tid < thread_num_; tid++) {
    if (producer_threads_[tid].get() == nullptr) continue;
    producer_threads_[tid]->join();
    producer_threads_[tid].reset();
  }
  loader_->Destroy();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (free_cells_.size() != 0) {
      delete free_cells_.front();
      free_cells_.pop();
    }
  }
  while (queue_->Size() > 0) {
    CellType cell(nullptr, nullptr);
    if (queue_->Pop(&cell)) {
      delete cell.first;
      delete cell.second;
    } else {
      break;
    }
  }
  if (out_data_ != nullptr) {
    delete out_data_;
    out_data_ = nullptr;
  }
}

template<typename DType, typename SourceType>
void MultiThreadedIter<DType, SourceType>::BeforeFirst(void) {
  // stop all thread
  force_stopped_ = true;
  while (Next()) {}  // pop queue_
  for (size_t tid = 0; tid < thread_num_; tid++) {
    if (producer_threads_[tid].get() == nullptr) continue;
    producer_threads_[tid]->join();
    producer_threads_[tid].reset();
  }
  while (Next()) {}

  // reset producer
  before_first_();
  loader_->BeforeFirst();
  force_stopped_ = false;
  null_cell_num_ = 0;
  producer_threads_.resize(thread_num_);
  for (size_t tid = 0; tid < thread_num_; tid++) {
    producer_threads_[tid].reset(new std::thread([this, tid]() { producer_thread_body_(tid); }));
  }
}
}  // namespace dmlc
#endif  // DMLC_USE_CXX11
#endif  // DMLC_THREADEDITER_H_
