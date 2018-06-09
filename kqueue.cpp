#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/event.h>
#include <sys/time.h>

#include <chrono>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(sizeof(uintptr_t) >= sizeof(void (*)(void*)),
              "Cannot cast from pointer-to-function to integral type");

namespace kq
{
	struct payload_t
	{
		void* data;
		void (*fun)(void*);
	};

	void run_payload(void* udata)
	{
		if (!udata)
			return;
		payload_t* payload = static_cast<payload_t*>(udata);
		payload->fun(payload->data);
		delete payload;
	}

	template <typename F>
	auto dispatch(int kq, F f)
	{
		//using R = std::invoke_result_t<F>;
		using R = std::result_of_t<F(void)>;
		using Task = std::packaged_task<R()>;
		Task* task = new Task(f);
		auto fut = task->get_future();
		auto payload = new payload_t{.data = task, .fun = [](void* p) {
			                             Task* t = static_cast<Task*>(p);
			                             (*t)();
			                             delete t;
		                             }};

		// cannot trigger user event immediately in same change
		struct kevent ke[2] = {};

		ke[0].ident = reinterpret_cast<uintptr_t>(&run_payload);
		ke[0].filter = EVFILT_USER;
		ke[0].flags = EV_ADD | EV_ONESHOT | EV_UDATA_SPECIFIC;
		ke[0].udata = payload;

		ke[1].ident = reinterpret_cast<uintptr_t>(&run_payload);
		ke[1].filter = EVFILT_USER;
		ke[1].flags = EV_UDATA_SPECIFIC;
		ke[1].fflags = NOTE_TRIGGER;
		ke[1].udata = payload;

		if (kevent(kq, ke, 2, nullptr, 0, nullptr))
			perror(__func__), abort();
		return fut;
	}

	const uintptr_t cancel_token = -1;

	void cancel(int kq) noexcept
	{
		struct kevent ke[2] = {};

		ke[0].ident = cancel_token;
		ke[0].filter = EVFILT_USER;
		ke[0].flags = EV_ADD;

		ke[1].ident = cancel_token;
		ke[1].filter = EVFILT_USER;
		ke[1].fflags = NOTE_TRIGGER;

		if (kevent(kq, ke, 2, nullptr, 0, nullptr) == -1)
			perror(__func__), abort();
	}
} // namespace kq

int main()
{
	using namespace std::chrono_literals;
	const int kq = kqueue();
	if (kq == -1)
		perror("kqueue"), abort();
	std::thread t([kq] {
		constexpr int N = 10;
		struct kevent ke[N];
		while (1)
		{
			//std::this_thread::sleep_for(100ms);
			const int ne = kevent(kq, nullptr, 0, ke, N, nullptr);
			if (ne == -1)
				perror(__func__), abort();
			printf("thread woke from kevent with %i events\n", ne);
			for (int e = 0; e < ne; ++e)
			{
				if (ke[e].ident == reinterpret_cast<uintptr_t>(&kq::run_payload))
					kq::run_payload(ke[e].udata);
				else if (ke[e].ident == kq::cancel_token)
					return; // leaks remaining tasks
				else
					printf("unknown ke.ident: %lu\n", ke[e].ident);
			}
		}
	});

	// push tasks
	std::vector<std::future<int>> futs;
	for (int i = 0; i < 5; ++i)
	{
		futs.push_back(kq::dispatch(kq, [i] {
			printf("Hello from task %3d\n", i);
			return i * i;
		}));
	}

	kq::cancel(kq);
	printf("joining thread...\n");
	t.join();
	printf("complete\n");

	for (auto& res : futs)
		printf("Res: %3d\n", res.get());

	close(kq);
}
