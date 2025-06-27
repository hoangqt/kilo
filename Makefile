TARGET=kilo

kilo: kilo.c
	$(CC) -o $(TARGET) kilo.c -Wall -W -pedantic -std=c99

clean:
	rm $(TARGET)

install:
	chmod +x $(TARGET)
	cp $(TARGET) /usr/local/bin

.PHONY: kilo clean install
