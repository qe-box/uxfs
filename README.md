
# uxfs

## About _uxfs_

_uxfs_ is a brigde to implement user interfaces (to what is called a
"controller") as a virtual filesystem.

I created _uxfs_ to have access to hardware on the Raspberry Pi.
Additional hardware comes usually with _python_ modules to access
them, but _python_ is not my preferred language (as you will see when
you look at _sense-hat.py_).  With the combination of _uxfs_ and
_sense-hat.py_ I'm able to access to access the Pi's Sense HAT
directly from within a shell script, e.g.

    echo 100 50 200 >/path/to/d/dot-21

Of course, this could have also been done by writing an appropriate
_python_ script.

So, what is _uxfs_ not?  Neither _uxfs_ nor its controller create any
magic that isn't already available.  They just create a virtual
filesystem as interface to it.  (It is said, that in UNIX everything
is a file.)

So what does "_... is a bridge ..._" mean?

 1. _uxfs_ interfaces with _libfuse_ to expose a virtual filesystem to
    user processes.

 2. Read-write operations are send to _uxfs_'s controller and are
    processed there.

 3. _uxfs_ itself does not implement any of the controller's
    application logic.  It is just a relay and the controller has full
    control about what is going on (and must implement it).


## Compiling

You need _libfuse-dev_ to compile _uxfs_:

    sudo apt-get install libfuse-dev fuse

(Of course you need a C compiler and linker too, but I think that's
obvious, right?)  You can compile the source with

    CFLAGS="-D_FILE_OFFSET_BITS=64 -I/usr/include/fuse"
    gcc -O2 -Wall $CFLAGS -c -o uxfs.o uxfs.c
    gcc -o uxfs uxfs.o -lfuse -pthread

or you run `make` if you have that installed too.


## Using

If you have a Raspberry Pi Sense HAT or _python_'s Sense HAT Emulator
module (`from sense_emu import SenseHat`) installed you can try

    mkdir -p x  &&  ./uxfs ./x -- ./sense-hat.py

and then

    echo 100 50 200 >x/d/dot-21
    echo "Hello World!" >x/print/text

in another shell.  You might run also run `./random.awk x`.  Use

    echo >x/shutdown

to terminate the controller (and the virtual filessystem).

_ux-test-stdio_ and _ux-test-main_ are also examples but more boring
because they don't interact with any hardware.  Start it with

    mkdir -p x  &&  ./uxfs ./x -- ./ux-test-stdio

or

    mkdir -p x  &&  ./ux-test-main

_ux-test-main_ and _ux-test-stdio_ do the same thing and the only
difference is their invocation and how they talk to _uxfs_.  Interact
with the files and watch _ux-stdio-test_'s output to get an idea of
_uxfs_'s protocol.  Look into the script code for more information and
read the manpage.  To test a more advanced example run

    mkdir x/test
    echo 3 >x/test/a
    echo 4 >x/test/b
    cat x/test/c

Again, use

    echo >x/shutdown

to terminate the program.

_uxfs_ and the controller don't have to run on the same host.  Setups
like

    ./uxfs ./x -- ssh -l pi pi400 "/path/to/uxfs/sense-hat.py"

are possible too.

