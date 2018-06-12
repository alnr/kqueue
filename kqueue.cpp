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
		payload_t* payload = new payload_t{.data = task, .fun = [](void* p) {
			                                   Task* t = static_cast<Task*>(p);
			                                   (*t)();
			                                   delete t;
		                                   }};

		// cannot trigger user event immediately in same change
		struct kevent ke[2] = {};

		ke[0].ident = reinterpret_cast<uintptr_t>(payload);
		ke[0].filter = EVFILT_USER;
		ke[0].flags = EV_ADD | EV_ONESHOT;

		ke[1].ident = reinterpret_cast<uintptr_t>(payload);
		ke[1].filter = EVFILT_USER;
		ke[1].fflags = NOTE_TRIGGER;

		if (kevent(kq, ke, 2, nullptr, 0, nullptr))
			perror(__func__), abort();
		return fut;
	}

	int cancel(int kq) noexcept
	{
		struct kevent ke[2] = {};

		ke[0].ident = 0;
		ke[0].filter = EVFILT_USER;
		ke[0].flags = EV_ADD;

		ke[1].ident = 0;
		ke[1].filter = EVFILT_USER;
		ke[1].fflags = NOTE_TRIGGER;

		if (kevent(kq, ke, 2, nullptr, 0, nullptr) == -1)
			return perror(__func__), errno;
		return 0;
	}
} // namespace kq

int main()
{
	using namespace std::chrono_literals;
	const int kq = kqueue();
	if (kq == -1)
		return perror("kqueue"), errno;
	std::thread t([kq] {
		constexpr int N = 10;
		struct kevent ke[N];
		while (1)
		{
			const int ne = kevent(kq, nullptr, 0, ke, N, nullptr);
			if (ne == -1)
				return perror(__func__), errno;
			printf("thread woke from kevent with %i events\n", ne);
			for (int e = 0; e < ne; ++e)
			{
				switch (ke[e].filter)
				{
				case EVFILT_USER:
					if (!ke[e].ident)
						return 0; // leaks remaining tasks
					else
						kq::run_payload(reinterpret_cast<kq::payload_t*>(ke[e].ident));
					break;
				default:
					fprintf(stderr, "unknown kevent filter: %u\n", ke[e].filter);
					break;
				}
			}
		}
		return 0;
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
