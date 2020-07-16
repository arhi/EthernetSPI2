/*
 * Allow ESP32-CAM module to talk to W5500 Ethernet module trough HSPI
 * as SPI (VSPI) pins are used to connect the camera module
 * 
 * Assembled from 
 *  - Arduino Ethernet library
 *  - ESP32-CAM example "CameraWebServer"
 *  
 * My work - public domain
 * Whatever is copied from those two is whatever their licence is
 * 
 * 
 * Hardware
 * ESP32-CAM AI-Thinker module (changing the CAMERA PINS should work with any other esp32-cam, but I have not tested)
 * W5500 Ethernet module (I removed the pieces for 5100 and 5200 from the library as I don't need them)
 * 
 * GPI0 to GND and then reset to program
 * GPI1 and GPI3 to your TX, RX uart for programming
 * You need to disable power to the W5500 in order to program ESP32 (no idea why)
 * 
 * ESP32-CAM  -> W5500
 * GPIO12     -> MISO
 * GPIO13     -> MOSI
 * GPIO14     -> SCLK
 * GPIO2      -> SCS
 * GND        -> GND
 * 
 * I am powering both from same externam 5V source
 * 
 * Since GPIO2 is used for chip select this should not affect SD card operations
 * as SD card is also using HSPI spi interface
 * 
 */


#include <SPI.h>
#include <EthernetSPI2.h>
#include "esp_camera.h"
#include "esp_timer.h"

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

//CAMERA PINS
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

//LEDS
#define LED 33
#define FLED 4

// Enter a MAC address and IP address for your controller below.
// The IP address will be dependent on your local network:
byte mac[] = {
  0x02, 0xAB, 0xCD, 0xEF, 0x00, 0x01
};
//IPAddress ip(192, 168, 89, 11);
IPAddress ip(192, 168, 89, 11);

// Initialize the Ethernet server library
// with the IP address and port you want to use
// (port 80 is default for HTTP):
EthernetServer server(80);

void setup() {
  pinMode(LED, OUTPUT);
  pinMode(FLED, OUTPUT);
  
  // Open serial communications and wait for port to open:
  Serial.begin(115200);

  // configure camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if(psramFound()){
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  //initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);//flip it back
    s->set_brightness(s, 1);//up the blightness just a bit
    s->set_saturation(s, -2);//lower the saturation
  }
  //drop down frame size for higher initial frame rate
  //s->set_framesize(s, FRAMESIZE_QVGA);
  s->set_framesize(s, FRAMESIZE_SVGA);




  Ethernet.init(2);
  //Ethernet.begin(mac, ip); // start the Ethernet connection and the server:
  Ethernet.begin(mac); 

  // Check for Ethernet hardware present
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println("Ethernet shield was not found.  Sorry, can't run without hardware. :(");
    while (true) {
      delay(200);
      digitalWrite(LED, HIGH);
      digitalWrite(FLED, HIGH);
      delay(200);
      digitalWrite(LED, LOW);
      digitalWrite(FLED, LOW);
    }
  }
  while (Ethernet.linkStatus() == LinkOFF) {
    Serial.println("Ethernet cable is NOT connected.");
      delay(500);
      digitalWrite(LED, HIGH);
      digitalWrite(FLED, HIGH);
      delay(500);
      digitalWrite(LED, LOW);
      digitalWrite(FLED, LOW);    
  }

  Serial.println("Ethernet cable is now connected.");
  
  server.begin();
  delay(500);
  Serial.print("server is at ");
  Serial.println(Ethernet.localIP());
}


void stream_h(EthernetClient *ec){
    camera_fb_t * fb = NULL;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char * part_buf[64];
    boolean error_sending = false;
    int bytes_sent;
    int jpg_od, jpg_jos;

    ec->println("HTTP/1.1 200 OK");
    ec->print("Content-Type: ");
    ec->println(_STREAM_CONTENT_TYPE);
    ec->println("Access-Control-Allow-Origin: *");
    ec->println();

    while(true){
       error_sending = false;
       fb = esp_camera_fb_get();
       if (!fb) {
           Serial.println("Camera capture failed");
           return;
       }

       if(fb->format != PIXFORMAT_JPEG){ // if not already jpg in framebuffer, convert to jpg
         bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
         esp_camera_fb_return(fb);
         fb = NULL;
         if(!jpeg_converted){
           Serial.println("JPEG compression failed");
           return;
         }
       } else {
         _jpg_buf_len = fb->len;
         _jpg_buf = fb->buf;
       }

       size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
       bytes_sent = ec->write((const char *)part_buf, hlen);
       if (bytes_sent != hlen) error_sending = true;

       // Send JPG data in 2kb chunks
       jpg_od  = 0;
       jpg_jos = _jpg_buf_len;
       while (jpg_jos > 0){
         if (jpg_jos > 2048) {
           bytes_sent = ec->write( (const char *)&_jpg_buf[jpg_od] , 2048);
           jpg_jos -= 2048;
           jpg_od += 2048;
         } else {
           bytes_sent = ec->write( (const char *)&_jpg_buf[jpg_od] , jpg_jos);
           jpg_jos = 0;
           jpg_od = 0;
         }
       }

       bytes_sent = ec->write(_STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
       if (bytes_sent != strlen(_STREAM_BOUNDARY)) error_sending = true;

       // release buffers
       if(fb){
         esp_camera_fb_return(fb);
         fb = NULL;
         _jpg_buf = NULL;
        } else if(_jpg_buf){
          free(_jpg_buf);
          _jpg_buf = NULL;
        }

       if (error_sending) break; // if client disconnected do not send any more JPG's
    }
}


void loop() {
  boolean biocr;
  boolean biolf;
  boolean doubleblank;
  boolean singleblank;

  // listen for incoming clients
  EthernetClient client = server.available();
  if (client) {
    Serial.println("new client");
    // an http request ends with a blank line
    doubleblank = false;
    singleblank = true;
    biocr = false;
    biolf = false;

    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        //Serial.write(c);

        if (c == '\n'){ //LF
          if (biolf){
            doubleblank = true; // \n\n
            singleblank = true;
          }
          if (biocr){
            if (singleblank) doubleblank = true; // \n\r\n
            singleblank = true;
          }
          biolf = true;
          biocr = false;
        } else if (c == '\r'){ //CR
          if (biocr){
            doubleblank = true; //\r\r
            singleblank = true;
          }
          if (biolf){
            // \n\r
          }
          
          biolf = false;
          biocr = true;          
        } else {
          biolf = false;
          biocr = false;
          singleblank = false;
          doubleblank = false;
        }

        if (doubleblank) {
          stream_h(&client); // stream the jpeg's
          singleblank = false;
          doubleblank = false;          
          break;
        }
      }
    }
    delay(10);
    client.stop();
    Serial.println("client disconnected");
  }
  delay(10);
}
