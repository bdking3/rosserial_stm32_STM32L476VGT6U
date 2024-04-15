#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <chrono>

#include <thread>
#include <mutex>

using namespace std::chrono_literals;

class FAKE_DMA_UART;
void HAL_UART_TxCpltCallback(FAKE_DMA_UART *huart);

enum {
  HAL_OK,
  HAL_BUSY,
};

class FAKE_DMA_UART {
public:
  std::thread t;
  std::mutex m;
  bool run = true;

  uint8_t *buf = nullptr;
  size_t len = 0;

  FAKE_DMA_UART() {
    this->t = std::thread(FAKE_DMA_UART::_work, this);
  }

  ~FAKE_DMA_UART() {
    if (this->run) {
      this->run = false;
      this->t.join();
    }
  }

  static void _work(FAKE_DMA_UART *self)
  {
    self->work();
  }

  bool isReady() {
    std::unique_lock<std::mutex> l(this->m, std::defer_lock);
    if (l.try_lock()) {
      return (this->buf == nullptr && this->len == 0);
    }
    return false;
  }

  void work()
  {
    printf("Work start\n");
    std::unique_lock<std::mutex> l(this->m, std::defer_lock);
    while (this->run) {
      l.lock();
      if (this->buf && this->len) {
        l.unlock();
        size_t n = 0;
        printf("DMA START:\n");
        while (n < this->len) {
          printf("%02x", this->buf[n++]);
          std::this_thread::sleep_for(8.68us);
        }
        printf("\nDMA COMPLETE.\n", this->len, this->buf);
        l.lock();
        this->buf = nullptr;
        this->len = 0;
        l.unlock();
      } else {
        l.unlock();
        std::this_thread::sleep_for(100ms);
      }
      HAL_UART_TxCpltCallback(this);
    }
  }

  int write(uint8_t *buf, size_t len)
  {
    std::unique_lock<std::mutex> l(this->m, std::defer_lock);
    if (l.try_lock()) {
      if (this->buf == nullptr && this->len == 0) {
        this->buf = buf;
        this->len = len;
        return HAL_OK;
      }
      l.unlock();
    }
    printf("HAL_BUSY!\n");
    return HAL_BUSY;
  }
};

int HAL_UART_Transmit_DMA(FAKE_DMA_UART *huart, uint8_t *buf, size_t len)
{
  return huart->write(buf, len);
}

// Copy data from variable into a byte array
template<typename A, typename V>
static void varToArr(A arr, const V var)
{
  for (size_t i = 0; i < sizeof(V); i++)
    arr[i] = (var >> (8 * i));
}

// Copy data from a byte array into variable
template<typename V, typename A>
static void arrToVar(V& var, const A arr)
{
  var = 0;
  for (size_t i = 0; i < sizeof(V); i++)
    var |= (arr[i] << (8 * i));
}

inline void HAL_Yield()
{
  std::this_thread::sleep_for(10ms);
}

class STM32Hardware {
  protected:
    const static uint16_t tbuflen = 512;
    uint8_t tbuf[tbuflen];
    uint32_t twind, tfind;

  public:
  FAKE_DMA_UART *huart;
    STM32Hardware():
      twind(0), tfind(0){
    }
  
    void flush(void){
      static bool mutex = false;

      if((huart->isReady()) && !mutex){
        mutex = true;

        if(twind != tfind){
          uint16_t len = 0;
          if(tfind < twind){
            len = twind - tfind;
            // If using any kind of RTOS or scheduling, instead of spin-loop you should yield/sleep as appropriate ;-)
            printf("Try write %d\n", len);
            while (HAL_UART_Transmit_DMA(huart, &(tbuf[tfind]), len) == HAL_BUSY) {
              HAL_Yield();
            }
          }else{
            len = tbuflen - tfind;
            printf("Try write %d\n", len);
            while (HAL_UART_Transmit_DMA(huart, &(tbuf[tfind]), len) == HAL_BUSY) {
              HAL_Yield();
            }
            printf("Try write %d\n", twind);
            while (HAL_UART_Transmit_DMA(huart, (uint8_t *)&tbuf, twind) == HAL_BUSY) {
              HAL_Yield();
            }
          }
          tfind = twind;
        }
        mutex = false;
      }
    }

    void write(uint8_t* data, int length){
      int n = length;
      n = n <= tbuflen ? n : tbuflen;

      int n_tail = n <= tbuflen - twind ? n : tbuflen - twind;
      memcpy(&(tbuf[twind]), data, n_tail);
      twind = (twind + n) & (tbuflen - 1);

      if(n != n_tail){
        memcpy(tbuf, &(data[n_tail]), n - n_tail);
      }

      flush();
    }
};

class Msg {
public:
  Msg() = default;
  ~Msg() = default;
  const char* data;
  int serialize(unsigned char *outbuffer)
  {
    int offset = 0;
    uint32_t length_data = strlen(this->data);
    varToArr(outbuffer + offset, length_data);
    offset += 4;
    memcpy(outbuffer + offset, this->data, length_data);
    offset += length_data;
    return offset;
  }
};

template<class Hardware,
         int MAX_SUBSCRIBERS = 25,
         int MAX_PUBLISHERS = 25,
         int INPUT_SIZE = 512,
         int OUTPUT_SIZE = 512>
class NodeHandler
{
public:
  const uint8_t MODE_PROTOCOL_VER   = 1;
  const uint8_t PROTOCOL_VER1       = 0xff; // through groovy
  const uint8_t PROTOCOL_VER2       = 0xfe; // in hydro
  const uint8_t PROTOCOL_VER        = PROTOCOL_VER2;
  const uint8_t MODE_SIZE_L         = 2;
  const uint8_t MODE_SIZE_H         = 3;
  const uint8_t MODE_SIZE_CHECKSUM  = 4;    // checksum for msg size received from size L and H
  const uint8_t MODE_TOPIC_L        = 5;    // waiting for topic id
  const uint8_t MODE_TOPIC_H        = 6;
  const uint8_t MODE_MESSAGE        = 7;
  const uint8_t MODE_MSG_CHECKSUM   = 8;    // checksum for msg and topic id

  Hardware hardware_{};
  uint8_t message_out[OUTPUT_SIZE] = {0};

  NodeHandler() = default;
  ~NodeHandler() = default;

  int publish(int id, Msg * msg)
  {
    /* serialize message */
    int l = msg->serialize(message_out + 7);

    /* setup the header */
    message_out[0] = 0xff;
    message_out[1] = PROTOCOL_VER;
    message_out[2] = (uint8_t)((uint16_t)l & 255);
    message_out[3] = (uint8_t)((uint16_t)l >> 8);
    message_out[4] = 255 - ((message_out[2] + message_out[3]) % 256);
    message_out[5] = (uint8_t)((int16_t)id & 255);
    message_out[6] = (uint8_t)((int16_t)id >> 8);

    /* calculate checksum */
    int chk = 0;
    for (int i = 5; i < l + 7; i++)
      chk += message_out[i];
    l += 7;
    message_out[l++] = 255 - (chk % 256);

    if (l <= OUTPUT_SIZE)
    {
      hardware_.write(message_out, l);
      return l;
    }
    else
    {
      printf("ERROR: Message from device dropped: message larger than buffer.");
      return -1;
    }
  }
};





FAKE_DMA_UART huart;
NodeHandler<STM32Hardware> nh;


void HAL_UART_TxCpltCallback(FAKE_DMA_UART *huart)
{
  nh.hardware_.flush();
}

int main(int argc, char const *argv[])
{
  // Setup
  nh.hardware_.huart = &huart;
  char hello[] = "1000:500:250:100:250:500:1000:1250";
  Msg msg;

  // Loop
  do {
    msg.data = hello;
    nh.publish(0, &msg);

    std::this_thread::sleep_for(100ms);
  } while (1);

  return 0;
}
