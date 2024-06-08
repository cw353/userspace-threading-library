Implementation of a user-thread library called `uthread`. 
This library provides the userspace portion of a many-to-one or
many-to-many threading model, depending on the number of kernel threads
requested at initialization.

This library was implemented as a solution to an operating systems course project designed by [Devin Pohly](https://github.com/devinpohly/). The project starter code can be found [here](https://github.com/devinpohly/csci455-project2). The implementation has been published with his permission after some modifications.

## Files

- `uthread.h` and `uthread.c`: contain the userspace threading functions implemented for this project. The header file
  defines the API that the `uthread` library provides.
- `kthread.h` and `kthread.c`: abstraction of kernel threads, which are primarily 
  wrappers for Pthreads functions. Provided as starter code; modified/extended slightly for this implementation.
- `queue.h` and `queue.c`: simple implementation of a linked-list queue, provided as starter code.
- Tests designed to verify each step, provided with the project.

# Testing
The tests can be run in order by using the `test` target from the Makefile
(i.e. `make test`). The `vtest` target will run the same tests in
Valgrind.  Each of these targets will stop at the first test which fails
and will output "success!" when all tests succeed.
The `ptest` target will run the tests using preemption, and the `pvtest` target will do the same but in Valgrind.
