#!/usr/bin/env python

import sys
import os
import fileinput
import time
import threading
import traceback
from sense_emu import SenseHat

#
# Sense HAT API Ref: https://pythonhosted.org/sense-hat/api/
#

sense = SenseHat()
buf = ""


# class line:
#     def __init__(self, line):
#         self.line = line;
#         self.words = line.rstrip().split(" ");
# 
#     def get():
#         try:
#             if len(self.args) > 0:
#                 s = self.args[0];
#                 self.args.pop(0);
#         except Exception as ex:
#             traceback.print_exc(file=sys.stderr)
# 
#         return s
# 
#     def getint(lower=0, upper=0):
#         v = 0;
#         try:
#             s = self.get()
#             if type(s) == str:
#                 v = int(s)
#         except Exception as ex:
#             traceback.print_exc(file=sys.stderr)
# 
#         v = lower if v < lower else v
#         v = upper if upper > lower  and  upper < v else v
# 
#         return v
# 

#
# Input Conversion
#

def c_next(args):
    s = ""
    try:
        if type(args) == list:
            if len(args) > 0:
                s = args[0]
                args.pop(0)
    except Exception as ex:
        traceback.print_exc(file=sys.stderr)

    return s


def c_int(s, lower=0, upper=0):
    v = 0;
    try:
        if type(s) == str:
            v = int(s)
        elif type(s) == float:
            v = int(s)
    except Exception as ex:
        traceback.print_exc(file=sys.stderr)

    v = lower if v < lower else v
    v = upper if upper > lower  and  upper < v else v

    return v


def c_bin(s, upper=0):
    v = 0;
    try:
        v = c_int(s)
    except Exception as ex:
        traceback.print_exc(file=sys.stderr)

    if upper > 0:
        v = v % upper

    return v


def c_num(s, lower=0, upper=0):
    v = 0;
    try:
        if type(s) == str:
            v = float(s)
        elif type(s) == int:
            v = float(s)
    except Exception as ex:
        traceback.print_exc(file=sys.stderr)

    v = lower if v < lower else v
    v = upper if upper > lower  and  upper < v else v

    return v


def c_rgb(r, g, b):
    rgb = [ 255, 255, 255 ]
    try:
        rgb = [ c_int(r, lower=0, upper=255),
                    c_int(g, lower=0, upper=255),
                    c_int(b, lower=0, upper=255) ]
    except Exception as ex:
        traceback.print_exc(file=sys.stderr)

    return rgb


#
# Text Display
#

class display:
    def __init__(self):
        self.updater = None
        self.reset()

    def reset(self, clear = True):
        self.buffer = ""
        self.repeat = 0
        self.position = 0
        self.updateDelay = .3
        self.updateDark = .05

        self.timerOff()
        if clear:
            sense.clear()

    def timerOff(self):
        if self.updater != None:
            self.updater.cancel()
            self.updater = None

    def timerStart(self):
        if self.updater == None:
            self.updater = threading.Timer(self.updateDelay, self.update)
            self.updater.start()

    def update(self):
        self.timerOff()
        if self.position >= len(self.buffer):
            sense.show_letter(" ");
            return

        c = self.buffer[ self.position ]
        if (c <= " "):
            c = "_"

        self.position = self.position + 1
        sense.show_letter(" ")
        time.sleep(self.updateDark)
        sense.show_letter(c)
        self.timerStart()

    def setText(self, s):
        if s == "":
            self.reset(1)
        else:
            self.buffer = s;
            self.position = 0;
            self.timerStart()



#
# uxfs Controller methods
#

def doInit():
    dn = "/d"
    s = dn + " d\n" + \
        dn + "/set w\n";
    for i in range(0, 8):
        for j in range(0, 8):
            s = s + dn + "/dot-" + str(i) + str(j) + " rw\n"
    
    print("+OK; DIR\n" +
          s +
          "/temp r\n" \
          "/hum r\n" \
          "/press r\n" \
          "/set w\n" \
          "/shutdown w\n" \
          "/print d\n" \
          "/print/text w\n" \
          "/print/fgcolor w\n" \
          "/print/bgcolor w\n" \
          "/print/delay w\n" \
          "/print/blank w\n" \
          "/user drw\n" \
          ".")

def doRequest(fn):
    if fn == "/temp":
        data = "%.2f C" % ( sense.get_temperature_from_pressure())
    elif fn == "/hum":
        data = "%.2f %%rH" % ( sense.get_humidity())
    elif fn == "/press":
        data = "%.2f mb" % ( sense.get_pressure())
    elif fn.startswith("/d/"):
        fn = fn[3:]
        if fn.startswith("dot-") and len(fn) == 6:
            x = c_bin(fn[4], upper=8)
            y = c_bin(fn[5], upper=8)
            rgb = sense.get_pixel(x, y)
            data = "%d %d %d" % (rgb[0], rgb[1], rgb[2])
        else:
            data = "0 0 0"
    else:
        data = "unkown file"

    print("+OK\n" \
        + data + "\n" \
        ".");

def doData(fn, data):
    if fn == "/shutdown":
        print("+OK; QUIT")
        return

    if fn == "/set":
        for line in data:
            par = line.rstrip().split(" ")
            if len(par) != 5:
                continue

            x = c_bin(c_next(par), upper=8)
            y = c_bin(c_next(par), upper=8)
            sense.set_pixel(x, y, c_rgb(c_next(par), c_next(par),
                                c_next(par)) )

    elif fn.startswith("/d/"):
        if len(data) > 0:
            par = data[0].rstrip().split(" ")
            fn = fn[3:]
            rgb = c_rgb(c_next(par), c_next(par), c_next(par))
            if fn == "set":
                sense.clear(rgb)
            elif fn[0:4] == "dot-":
                x = c_bin(fn[4], upper=8)
                y = c_bin(fn[5], upper=8)
                sense.set_pixel(x, y, rgb)

    elif fn.startswith("/print/"):
        T = ""
        for line in data:
            T = T + line + "\n"
            
        fn = fn[7:]
        if fn == "text":
            text.setText( c_next(data).rstrip())
        elif fn == "delay":
            text.updateDelay = c_num( c_next(data))
        elif fn == "dark":
            text.updateDark = c_num( c_next(data))

    print("+OK")



def readData():
    global inp;

    data = list()
    for line in inp:
        line = line.rstrip()
        if line == ".":
            break
        elif line.startswith("."):
            line = line[1:]

        data.append(line)

    return data



#text.setText("Hello World!")

def mainloop():
    global inp;
    for line in inp:
        args = line.rstrip().split(" ")
        if len(args) == 0:
            continue

        msg = c_next(args).upper()
        if msg == "INIT":
            doInit()

        elif msg == "QUIT":
            print("+OK")
            sys.stdout.flush()
            return (0)

        elif msg == "READ":
            fn = c_next(args)
            doRequest(fn)

        elif msg == "WRITE":
            fn = c_next(args)
            data = readData()
            doData(fn, data)

        elif msg == "FILEOP":
            data = readData()
            print("+OK")

        else:
            print("-ERR: unknown command: %s" % (msg));

        sys.stdout.flush()


try:
    text = display()
    inp = fileinput.input();
    mainloop()

except Exception as ex:
    print("-ERR: exception: " + str(ex))
    sys.stdout.flush()
    traceback.print_exc(file=sys.stderr)
    sys.exit(1)
 
 
