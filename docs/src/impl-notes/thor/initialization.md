# Initialization

Thor's initialization consists of multiple stages:

1. **Early initialization** (in `thorInitialize()`)**.**
	This stage performs rudimentary initialization of the system
	to enable the use of basic kernel infrastructure.
	Since it runs before global constructors, code in this stage
	cannot use global variables unless they are `constinit`
	or defined as static singletons inside functions.

	The early initialization stage is split into the following steps:

	1. **Initialization of basic features the CPU architecture**
		(in `initializeArchitecture()`)**.**
		Among other (architecture-dependent) things, this step
		initializes access to CPU-local data (via `getCpuData()`).

	2. **Initialization of debug sinks and loggers.**
		This is done early such that other initialization code can use
		Thor's usual logging infrastructure
		(including `infoLogger()` and `panicLogger()`).

	3. **Initialization of the memory management subsystem.**

2. **Running global constructors.**
	This stage runs global C++ constructors.
	Among other things, global constructors are used to build
	the initgraph data structure that is used in the next stage.

3. **Running the so-called initgraph** (in `thorMain()`)**.**
	The initgraph is a data structure that
	consists of tasks and dependencies between tasks.
	Tasks can define both dependencies and reverse dependencies;
	this way, the initgraph can easily be used to express dependencies
	between generic and subsystem-specific tasks.

	Thor does not run the entire initgraph at once; instead,
	the initgraph is split into multiple milestones:

	1. **Reaching the `tasking-available` milestone**.
		Early tasks of the initgraph run directly in `thorMain`,
		before multi-tasking or IRQ support is available.

	2. **Running the remaining initgraph on a kernel fiber**.
		Once the `tasking-available` milestone is reached,
		Thor enters multi-tasking mode and runs the remanining
		tasks of the initgraph in a fiber (= kernel thread).

		Since IRQ support is not available before `tasking-available`
		is reached, all tasks that depend on IRQs must depend
		(directly or indirectly) on `tasking-available`.
