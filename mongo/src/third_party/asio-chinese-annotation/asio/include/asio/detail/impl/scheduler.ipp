//
// detail/impl/scheduler.ipp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_DETAIL_IMPL_SCHEDULER_IPP
#define ASIO_DETAIL_IMPL_SCHEDULER_IPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "asio/detail/config.hpp"

#include "asio/detail/concurrency_hint.hpp"
#include "asio/detail/event.hpp"
#include "asio/detail/limits.hpp"
#include "asio/detail/reactor.hpp"
#include "asio/detail/scheduler.hpp"
#include "asio/detail/scheduler_thread_info.hpp"

#include "asio/detail/push_options.hpp"

namespace asio {
namespace detail {

struct scheduler::task_cleanup
{
  //scheduler::do_run_one  scheduler::do_poll_one  scheduler::do_wait_one��ִ������
  ////�����̵߳�˽�ж��з���ȫ�ֶ����У�Ȼ����task_operation_�����һ���߳�˽�ж��еĽ�����
  //ÿ��ִ��epoll_reactor::runȥ��ȡepoll��Ӧ���¼��б���ʱ�򶼻�ִ�и�cleanup
  ~task_cleanup()
  {
    if (this_thread_->private_outstanding_work > 0)
    {
      asio::detail::increment(
          scheduler_->outstanding_work_,
          this_thread_->private_outstanding_work);
    }
    this_thread_->private_outstanding_work = 0;

    // Enqueue the completed operations and reinsert the task at the end of
    // the operation queue.
    lock_->lock();
	//�����̵߳�˽�ж��з���ȫ�ֶ����У�Ȼ����task_operation_�����һ���߳�˽�ж��еĽ�����
	//task_operation_���һ���߳�˽�ж��еĽ�����
    scheduler_->task_interrupted_ = true;
    scheduler_->op_queue_.push(this_thread_->private_op_queue);

	//ע���������һ�������op task_operation_��ȫ�ֶ���
    scheduler_->op_queue_.push(&scheduler_->task_operation_);
  }

  scheduler* scheduler_;
  //�����������
  mutex::scoped_lock* lock_;
  thread_info* this_thread_;
};

struct scheduler::work_cleanup
{
  //scheduler::do_run_one  scheduler::do_poll_one  scheduler::do_wait_one��ִ������
  
  ~work_cleanup()
  {
    if (this_thread_->private_outstanding_work > 1)
    {
      asio::detail::increment(
          scheduler_->outstanding_work_,
          this_thread_->private_outstanding_work - 1);
    }
    else if (this_thread_->private_outstanding_work < 1)
    {
      scheduler_->work_finished();
    }
    this_thread_->private_outstanding_work = 0;

	//�����̵߳�˽�ж��з���ȫ�ֶ����� 
#if defined(ASIO_HAS_THREADS)
    if (!this_thread_->private_op_queue.empty())
    {
      lock_->lock();
      scheduler_->op_queue_.push(this_thread_->private_op_queue);
    }
#endif // defined(ASIO_HAS_THREADS)
  }

  scheduler* scheduler_;
  mutex::scoped_lock* lock_;
  thread_info* this_thread_;
};

scheduler::scheduler(
    asio::execution_context& ctx, int concurrency_hint)
  : asio::detail::execution_context_service_base<scheduler>(ctx),
    one_thread_(concurrency_hint == 1 //Ĭ��-1,���Բ���Ϊtrue
        || !ASIO_CONCURRENCY_HINT_IS_LOCKING(
          SCHEDULER, concurrency_hint)
        || !ASIO_CONCURRENCY_HINT_IS_LOCKING(
          REACTOR_IO, concurrency_hint)),
    mutex_(ASIO_CONCURRENCY_HINT_IS_LOCKING(
          SCHEDULER, concurrency_hint)),
    task_(0),
    task_interrupted_(true),
    outstanding_work_(0),
    stopped_(false),
    shutdown_(false),
    concurrency_hint_(concurrency_hint)
{
  ASIO_HANDLER_TRACKING_INIT;
}

//���ٶ����ϵ�op
void scheduler::shutdown()
{
  mutex::scoped_lock lock(mutex_);
  shutdown_ = true;
  lock.unlock();

  // Destroy handler objects.
  while (!op_queue_.empty())
  {
    operation* o = op_queue_.front();
    op_queue_.pop();
    if (o != &task_operation_)
      o->destroy();
  }

  // Reset to initial state.
  task_ = 0;
}

void scheduler::init_task()
{
  mutex::scoped_lock lock(mutex_);
  if (!shutdown_ && !task_)
  {
    task_ = &use_service<reactor>(this->context());
    op_queue_.push(&task_operation_);
    wake_one_thread_and_unlock(lock);
  }
}

//mongodb�е�accept�����ӹ���TransportLayerASIO::start()->io_context::run->scheduler::run�е���
std::size_t scheduler::run(asio::error_code& ec)
{
  ec = asio::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.private_outstanding_work = 0;
  //���е�
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

  std::size_t n = 0;
  for (; do_run_one(lock, this_thread, ec); lock.lock())
    if (n != (std::numeric_limits<std::size_t>::max)())
      ++n;
  
  return n;
}

std::size_t scheduler::run_one(asio::error_code& ec)
{
  ec = asio::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.private_outstanding_work = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

  return do_run_one(lock, this_thread, ec);
}

//accept����
//TransportLayerASIO::_acceptConnection->basic_socket_acceptor::async_accept
//->start_accept_op->epoll_reactor::post_immediate_completion

//��ͨread write op�����������
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::post(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::post_immediate_completion
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::dispatch(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::do_dispatch

//��ͨ��дread write �Ӷ��л�ȡopִ������
//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one->scheduler::do_wait_one����
//mongodb��ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_one_for->io_context::run_one_until->schedule::wait_one


//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one->scheduler::do_wait_one����
//mongodb��ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_one_for->io_context::run_one_until->schedule::wait_one
std::size_t scheduler::wait_one(long usec, asio::error_code& ec)
{
  ec = asio::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.private_outstanding_work = 0;
  //�߳���ӵ�top����
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

  //�Ӳ��������л�ȡ��Ӧ��operationִ�У������ȡ��operation��ִ�гɹ��򷵻�1�����򷵻�0
  return do_wait_one(lock, this_thread, usec, ec);
}

std::size_t scheduler::poll(asio::error_code& ec)
{
  ec = asio::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.private_outstanding_work = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

#if defined(ASIO_HAS_THREADS)
  // We want to support nested calls to poll() and poll_one(), so any handlers
  // that are already on a thread-private queue need to be put on to the main
  // queue now.
  
  if (one_thread_)
    if (thread_info* outer_info = static_cast<thread_info*>(ctx.next_by_key()))
      op_queue_.push(outer_info->private_op_queue);
#endif // defined(ASIO_HAS_THREADS)

  std::size_t n = 0;
  for (; do_poll_one(lock, this_thread, ec); lock.lock())
    if (n != (std::numeric_limits<std::size_t>::max)())
      ++n;
  return n;
}

std::size_t scheduler::poll_one(asio::error_code& ec)
{
  ec = asio::error_code();
  if (outstanding_work_ == 0)
  {
    stop();
    return 0;
  }

  thread_info this_thread;
  this_thread.private_outstanding_work = 0;
  thread_call_stack::context ctx(this, this_thread);

  mutex::scoped_lock lock(mutex_);

#if defined(ASIO_HAS_THREADS)
  // We want to support nested calls to poll() and poll_one(), so any handlers
  // that are already on a thread-private queue need to be put on to the main
  // queue now.
  if (one_thread_)
    if (thread_info* outer_info = static_cast<thread_info*>(ctx.next_by_key()))
      op_queue_.push(outer_info->private_op_queue);
#endif // defined(ASIO_HAS_THREADS)

  return do_poll_one(lock, this_thread, ec);
}

//scheduler::work_finished
void scheduler::stop()
{
  mutex::scoped_lock lock(mutex_);
  stop_all_threads(lock);
}

bool scheduler::stopped() const
{
  mutex::scoped_lock lock(mutex_);
  return stopped_;
}

void scheduler::restart()
{
  mutex::scoped_lock lock(mutex_);
  stopped_ = false;
}

void scheduler::compensating_work_started()
{
  thread_info_base* this_thread = thread_call_stack::contains(this);
  ++static_cast<thread_info*>(this_thread)->private_outstanding_work;
}

//accept����
//TransportLayerASIO::_acceptConnection->basic_socket_acceptor::async_accept
//->start_accept_op->epoll_reactor::post_immediate_completion

//��ͨread write op�����������
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::post(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::post_immediate_completion
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::dispatch(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::do_dispatch

//��ͨ��дread write �Ӷ��л�ȡopִ������
//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one
//->scheduler::do_wait_one����
//mongodb��ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_one_for
//->io_context::run_one_until->schedule::wait_one
void scheduler::post_immediate_completion(
    scheduler::operation* op, bool is_continuation)
{
#if defined(ASIO_HAS_THREADS)
  if (one_thread_ || is_continuation)
  {
    if (thread_info_base* this_thread = thread_call_stack::contains(this))
    {
      ++static_cast<thread_info*>(this_thread)->private_outstanding_work;
      static_cast<thread_info*>(this_thread)->private_op_queue.push(op);
      return;
    }
  }
#else // defined(ASIO_HAS_THREADS)
  (void)is_continuation;
#endif // defined(ASIO_HAS_THREADS)

  work_started();

//Ĭ�϶��̣߳�����������Ҫ�Զ��м���
  mutex::scoped_lock lock(mutex_);
  op_queue_.push(op);
  wake_one_thread_and_unlock(lock);
}

void scheduler::post_deferred_completion(scheduler::operation* op)
{
#if defined(ASIO_HAS_THREADS)
  if (one_thread_)
  {
    if (thread_info_base* this_thread = thread_call_stack::contains(this))
    {
      static_cast<thread_info*>(this_thread)->private_op_queue.push(op);
      return;
    }
  }
#endif // defined(ASIO_HAS_THREADS)
  //Ĭ�϶��̣߳�����������Ҫ�Զ��м���

  mutex::scoped_lock lock(mutex_);
  op_queue_.push(op);
  wake_one_thread_and_unlock(lock);
}

//epoll_reactor::cancel_ops  ~perform_io_cleanup_on_block_exit()����
void scheduler::post_deferred_completions(
    op_queue<scheduler::operation>& ops)
{
  if (!ops.empty())
  {
#if defined(ASIO_HAS_THREADS)
    if (one_thread_)
    {
      if (thread_info_base* this_thread = thread_call_stack::contains(this))
      {
        static_cast<thread_info*>(this_thread)->private_op_queue.push(ops);
        return;
      }
    }
#endif // defined(ASIO_HAS_THREADS)
	//Ĭ�϶��̣߳�����������Ҫ�Զ��м���

    mutex::scoped_lock lock(mutex_);
    op_queue_.push(ops); //scheduler.op_queue_
    wake_one_thread_and_unlock(lock);
  }
}


//accept����
//TransportLayerASIO::_acceptConnection->basic_socket_acceptor::async_accept
//->start_accept_op->epoll_reactor::post_immediate_completion

//��ͨread write op�����������
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::post(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::post_immediate_completion
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::dispatch(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::do_dispatch

//��ͨ��дread write �Ӷ��л�ȡopִ������
//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one->scheduler::do_wait_one����
//mongodb��ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_one_for->io_context::run_one_until->schedule::wait_one


//���
void scheduler::do_dispatch(
    scheduler::operation* op)
{
  work_started();
  //Ĭ�϶��̣߳�����������Ҫ�Զ��м���
  mutex::scoped_lock lock(mutex_);
  op_queue_.push(op);
  wake_one_thread_and_unlock(lock);
}

void scheduler::abandon_operations(
    op_queue<scheduler::operation>& ops)
{
  op_queue<scheduler::operation> ops2;
  ops2.push(ops);
}

//mongodb�е�accept�����ӹ���TransportLayerASIO::start()->io_context::run->scheduler::run�е���

//scheduler::run  scheduler::run_one ����
//����epoll��ȡ��Ӧ��д�¼���Ȼ���ڶ���op_queue_��ȡ��ͳһִ��
std::size_t scheduler::do_run_one(mutex::scoped_lock& lock,
    scheduler::thread_info& this_thread,
    const asio::error_code& ec)
{
  //stop_all_threads����Ϊtrue, Ϊtrue�󣬽����ٴ���epoll����¼����ο�scheduler::do_run_one
  while (!stopped_)
  {
    //ÿ�δ�op_queue����ʵ��һ�����������ó�ͷ�ڵ㣬Ȼ������
    if (!op_queue_.empty())
    { //�������Ѿ�û�п���ָ��ûص��ˣ������epoll_wait�ȴ���ȡ��Ӧ�¼��ص�
      // Prepare to execute first handler from queue.
      operation* o = op_queue_.front();
      op_queue_.pop();
      bool more_handlers = (!op_queue_.empty());

      if (o == &task_operation_) //����������op��˵����Ҫ������ȡһ��epoll�¼��ص�
      {
        task_interrupted_ = more_handlers;

		//�����ʣ�µ��������Ƕ��̻߳�������ʹ��wake_one_thread_and|unlock���Ի��ѿ������ߵ��̣߳�
        if (more_handlers && !one_thread_)
          wakeup_event_.unlock_and_signal_one(lock); //��ǰ����
        else
          lock.unlock();

        task_cleanup on_exit = { this, &lock, &this_thread };
//��scheduler::do_run_one��������ǰ�������task_cleanup�����������Ӷ���this_thread.private_op_queue���
//��scheduler.op_queue_
        (void)on_exit;

        // Run the task. May throw an exception. Only block if the operation
        // queue is empty and we're not polling, otherwise we want to return
        // as soon as possible.
        //scheduler::do_run_one->epoll_reactor::run
        //ͨ��epoll��ȡ���е������¼�op��ӵ�private_op_queue, ������ͨ��scheduler::poll_one scheduler::poll��ӵ�op_queue_
		//epoll_reactor::run
		task_->run(more_handlers ? 0 : -1, this_thread.private_op_queue);
		//����exit��ʱ��ִ��ǰ���on_exit���������Ӷ���this_thread.private_op_queue��ӵ�scheduler.op_queue_
      }
      else
      { //ȡ�������ϵ�һ��op�ص���op����ǰ���if����ӵã�������scheduler::runѭ��ִ��

	    //��ȡepoll_wait���ص�event��Ϣ����ֵ��set_ready_events add_ready_events
        std::size_t task_result = o->task_result_; 
		

        if (more_handlers && !one_thread_)
          wake_one_thread_and_unlock(lock);
        else
          lock.unlock();

        // Ensure the count of outstanding work is decremented on block exit.
        work_cleanup on_exit = { this, &lock, &this_thread };
		//����exit��ʱ��ִ��ǰ���on_exit���������Ӷ���this_thread.private_op_queue��ӵ�scheduler.op_queue_
        (void)on_exit;
		/*
		asio::detail::reactive_socket_recv_op<
		 asio::mutable_buffers_1, asio::detail::read_op
		 <
		  asio::basic_stream_socket<asio::generic::stream_protocol>, asio::mutable_buffers_1, asio::mutable_buffer const*, asio::detail::transfer_all_t, 
		 void mongo::transport::TransportLayerASIO::ASIOSession::opportunisticRead<asio::basic_stream_socket<asio::generic::stream_protocol
		 >, 
		 asio::mutable_buffers_1, mongo::transport::TransportLayerASIO::ASIOSourceTicket::fillImpl()::{lambda(mongo::Status const&, unsigned long)#1}
		>
		 
		  (
		   bool, asio::basic_stream_socket<asio::generic::stream_protocol>&, 
		   asio::mutable_buffers_1 const&, 
		   mongo::transport::TransportLayerASIO::ASIOSourceTicket::fillImpl()::{lambda(mongo::Status const&, unsigned long)#1}&&
		  )
		::{lambda(std::error_code const&, unsigned long)#1}> >::do_complete(void*, asio::detail::scheduler_operation*, std::error_code const, unsigned long) ()
		*/
        // Complete the operation. May throw an exception. Deletes the object.
        //epoll_reactor::descriptor_state::do_complete
        o->complete(this, ec, task_result); //������scheduler::runѭ��ִ��

        return 1;
      }
    }
    else
    { //����������Ϊ�գ����߳���ô����Ҫ�ȴ��� �ȴ������߳�ͨ��wake_one_thread_and_unlock����
      wakeup_event_.clear(lock);
      wakeup_event_.wait(lock);
    }
  }

  return 0;
}

//accept����
//TransportLayerASIO::_acceptConnection->basic_socket_acceptor::async_accept
//->start_accept_op->epoll_reactor::post_immediate_completion

//��ͨread write op�����������
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::post(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::post_immediate_completion
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::dispatch(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::do_dispatch

//��ͨ��дread write �Ӷ��л�ȡopִ������
//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one->scheduler::do_wait_one����
//mongodb��ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_one_for->io_context::run_one_until->schedule::wait_one

//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one->scheduler::do_wait_one����
//waitһ��ʱ��  

//�Ӳ��������л�ȡ��Ӧ��operationִ�У������ȡ��operation��ִ�гɹ��򷵻�1�����򷵻�0
std::size_t scheduler::do_wait_one(mutex::scoped_lock& lock,
    scheduler::thread_info& this_thread, long usec,
    const asio::error_code& ec)
{//ȡ�����е�һ��opִ��

  //stop_all_threads����Ϊtrue, Ϊtrue�󣬽����ٴ���epoll����¼����ο�scheduler::do_run_one
  if (stopped_)
    return 0;

  operation* o = op_queue_.front();
  if (o == 0) //�������Ϊ�գ���ȴ�usec
  {
    //�ȴ�������
    wakeup_event_.clear(lock);
    wakeup_event_.wait_for_usec(lock, usec);
    usec = 0; // Wait at most once.
    //��һ��������Ǽ����ж϶������Ƿ��п�ִ�е�op
    o = op_queue_.front();
  }

  if (o == &task_operation_) //op_queue_��������û��IO op����������ͨ��epoll_wait��ȡ����IO�¼���Ϣ��Ӧ��op�����Ӧ����
  {
    op_queue_.pop();
    bool more_handlers = (!op_queue_.empty());

    task_interrupted_ = more_handlers;

    if (more_handlers && !one_thread_)
      wakeup_event_.unlock_and_signal_one(lock);
    else
      lock.unlock();

    {
      task_cleanup on_exit = { this, &lock, &this_thread };
	  //����exit��ʱ��ִ��ǰ���on_exit���������Ӷ���this_thread.private_op_queue��ӵ�scheduler.op_queue_
        
      (void)on_exit;

      // Run the task. May throw an exception. Only block if the operation
      // queue is empty and we're not polling, otherwise we want to return
      // as soon as possible.
      //scheduler::do_run_one->epoll_reactor::run
      //ͨ��epoll��ȡ���е������¼�op��ӵ�private_op_queue, ������ͨ��scheduler::poll_one scheduler::poll��ӵ�op_queue_
      task_->run(more_handlers ? 0 : usec, this_thread.private_op_queue);
    }

    o = op_queue_.front();
    if (o == &task_operation_)
    {
      if (!one_thread_)
        wakeup_event_.maybe_unlock_and_signal_one(lock);
      return 0;
    }
  }

  if (o == 0)
    return 0;

  //����ִ��o�������o����reactor_op��complete_func
  op_queue_.pop();
  bool more_handlers = (!op_queue_.empty());

  std::size_t task_result = o->task_result_;

  if (more_handlers && !one_thread_)
    wake_one_thread_and_unlock(lock);
  else
    lock.unlock();

  // Ensure the count of outstanding work is decremented on block exit.
  
  work_cleanup on_exit = { this, &lock, &this_thread };
  //����exit��ʱ��ִ��ǰ���on_exit���������Ӷ���this_thread.private_op_queue��ӵ�scheduler.op_queue_
        
  (void)on_exit;

  // Complete the operation. May throw an exception. Deletes the object.
  /*
  asio::detail::reactive_socket_recv_op<
   asio::mutable_buffers_1, asio::detail::read_op
   <
	asio::basic_stream_socket<asio::generic::stream_protocol>, asio::mutable_buffers_1, asio::mutable_buffer const*, asio::detail::transfer_all_t, 
   void mongo::transport::TransportLayerASIO::ASIOSession::opportunisticRead<asio::basic_stream_socket<asio::generic::stream_protocol
   >, 
   asio::mutable_buffers_1, mongo::transport::TransportLayerASIO::ASIOSourceTicket::fillImpl()::{lambda(mongo::Status const&, unsigned long)#1}
  >
   
	(
	 bool, asio::basic_stream_socket<asio::generic::stream_protocol>&, 
	 asio::mutable_buffers_1 const&, 
	 mongo::transport::TransportLayerASIO::ASIOSourceTicket::fillImpl()::{lambda(mongo::Status const&, unsigned long)#1}&&
	)
  ::{lambda(std::error_code const&, unsigned long)#1}> >::do_complete(void*, asio::detail::scheduler_operation*, std::error_code const, unsigned long) ()
  */ //epoll_reactor::descriptor_state::do_complete
  o->complete(this, ec, task_result);

  return 1;  
}


//scheduler::poll_one  scheduler::poll
std::size_t scheduler::do_poll_one(mutex::scoped_lock& lock,
    scheduler::thread_info& this_thread,
    const asio::error_code& ec)
{
  //stop_all_threads����Ϊtrue, Ϊtrue�󣬽����ٴ���epoll����¼����ο�scheduler::do_run_one
  if (stopped_)
    return 0;

  operation* o = op_queue_.front();
  if (o == &task_operation_)
  {
    op_queue_.pop();
    lock.unlock();

    {
      task_cleanup c = { this, &lock, &this_thread };
	  //����exit��ʱ��ִ��ǰ���on_exit���������Ӷ���this_thread.private_op_queue��ӵ�scheduler.op_queue_
        
      (void)c;

      // Run the task. May throw an exception. Only block if the operation
      // queue is empty and we're not polling, otherwise we want to return
      // as soon as possible.
      task_->run(0, this_thread.private_op_queue);
    }

    o = op_queue_.front();
    if (o == &task_operation_)
    {
      wakeup_event_.maybe_unlock_and_signal_one(lock);
      return 0;
    }
  }

  if (o == 0)
    return 0;

  op_queue_.pop();
  bool more_handlers = (!op_queue_.empty());

  std::size_t task_result = o->task_result_;

  if (more_handlers && !one_thread_)
    wake_one_thread_and_unlock(lock);
  else
    lock.unlock();

  // Ensure the count of outstanding work is decremented on block exit.
  work_cleanup on_exit = { this, &lock, &this_thread };
  //����exit��ʱ��ִ��ǰ���on_exit���������Ӷ���this_thread.private_op_queue��ӵ�scheduler.op_queue_
  (void)on_exit;

  // Complete the operation. May throw an exception. Deletes the object.
  //epoll_reactor::descriptor_state::do_complete
  o->complete(this, ec, task_result);

  return 1;
}

//scheduler::stop����
void scheduler::stop_all_threads(
    mutex::scoped_lock& lock)
{
  //���ٴ���epoll����¼����ο�scheduler::do_run_one
  stopped_ = true;
  //��������wakeup_event_.wait���ߵȴ��߳�
  wakeup_event_.signal_all(lock);

  if (!task_interrupted_ && task_)
  {
    task_interrupted_ = true;
    task_->interrupt(); //epoll_reactor::interrupt
  }
}

//���Ի��ѿ������ߵ��̣߳�����ֱ���ͷ���
void scheduler::wake_one_thread_and_unlock(
    mutex::scoped_lock& lock)
{
  if (!wakeup_event_.maybe_unlock_and_signal_one(lock))
  {
    if (!task_interrupted_ && task_)
    {
      task_interrupted_ = true;
	  //epoll_reactor::interrupt
      task_->interrupt();
    }
    lock.unlock();
  }
}

} // namespace detail
} // namespace asio

#include "asio/detail/pop_options.hpp"

#endif // ASIO_DETAIL_IMPL_SCHEDULER_IPP