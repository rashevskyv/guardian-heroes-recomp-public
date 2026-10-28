#if REX_PLATFORM_SWITCH  // gh-edits: client threads
  {
    // One host thread per client: the guest callbacks mostly wait (hand-off to the game's mixer thread, 1.5-4 ms
    // each), and serving three clients one after another on the audio worker starved the last one.
    const uint32_t gen = ++gh_client_gen[index];
    auto self = std::make_shared<system::XHostThread*>(nullptr);
    auto* host = new system::XHostThread(gh_kernel_state, 128 * 1024, 0, [this, index, gen, self]() {
      auto* ts = (*self)->thread_state();
      uint64_t calls = 0, total_us = 0, max_us = 0, over5 = 0;
      auto last_print = std::chrono::steady_clock::now();
      while (worker_running_ && gh_client_gen[index] == gen) {
        auto r = rex::thread::Wait(client_semaphores_[index].get(), true, std::chrono::milliseconds(100));
        if (r != rex::thread::WaitResult::kSuccess) continue;
        uint32_t cb, arg;
        {
          auto lock = global_critical_region_.Acquire();
          if (gh_client_gen[index] != gen || !clients_[index].in_use) {
            // A newer registration of this slot: hand the credit I took back to it, and leave.
            if (clients_[index].in_use) client_semaphores_[index]->Release(1, nullptr);
            break;
          }
          cb = clients_[index].callback;
          arg = clients_[index].wrapped_callback_arg;
        }
        if (!cb) continue;
        uint64_t args[] = {arg};
        const auto t0 = std::chrono::steady_clock::now();
        function_dispatcher_->Execute(ts, cb, args, rex::countof(args));
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        ++calls;
        total_us += us;
        if (us > max_us) max_us = us;
        if (us > 5000) ++over5;
        if (t1 - last_print > std::chrono::seconds(5)) {
          last_print = t1;
          std::printf("[nx] audio client thread %zu: callbacks %llu, mean %llu us, max %llu us, %llu over 5 ms\n", (size_t)index,
                      (unsigned long long)calls, (unsigned long long)(total_us / calls), (unsigned long long)max_us,
                      (unsigned long long)over5);
          std::fflush(stdout);
          calls = total_us = max_us = over5 = 0;
        }
      }
      return 0;
    });
    *self = host;
    host->set_name("Audio Client " + std::to_string(index));
    host->Create();
  }
#endif
