CC = gcc
CFLAGS = -O2 -pthread -Wall -Wextra -g -D_GNU_SOURCE
LDFLAGS = -lcrypto -lpthread

TARGET = c_engine
SOURCE = c_engine.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

clean:
	rm -f $(TARGET)
	rm -rf blocks manifests

run: $(TARGET)
	./$(TARGET) /tmp/cengine.sock

test: $(TARGET)
	@echo "Starting C engine in background..."
	./$(TARGET) /tmp/cengine.sock &
	@sleep 1
	@echo "Engine started. Run 'python3 main.py' in another terminal."

.PHONY: all clean run test