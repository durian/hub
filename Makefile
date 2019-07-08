INCFILES=$(wildcard *.h)

LIBS = -lnanomsg -ldill

ALL = cloud-call hub cloudgps cloudlog

all: $(ALL)

cloud-call: cloud-call.o
	$(CC) -o $@ $^ $(LIBS) -ljansson

cloud-call.o: cloud.c $(INCFILES)
	$(CC) $(CFLAGS) -c $< -o $@

hub: hub.o
	$(CC) -o $@ $^ $(LIBS) -lpthread 

hub.o: hub.c $(INCFILES)
	$(CC) $(CFLAGS) -c $< -o $@

cloudgps: cloudgps.o
	$(CC) -o $@ $^ $(LIBS)

cloudgps.o: cloudgps.c $(INCFILES)
	$(CC) $(CFLAGS) -c $< -o $@

cloudlog: cloudlog.o
	$(CC) -o $@ $^ $(LIBS)

cloudlog.o: cloudlog.c $(INCFILES)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f  *.o *~ ${ALL}