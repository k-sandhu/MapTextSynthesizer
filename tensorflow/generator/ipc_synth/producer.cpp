#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <opencv2/opencv.hpp>
#include <signal.h>

#include "map_text_synthesizer.hpp"

extern "C" {
#include "prod_cons.h"
};

// Necessary for signal handler
void* g_buff;

void write_data(intptr_t buff, uint32_t height,
		const char* label, uint64_t img_sz, unsigned char* img_flat) {

  // Mark as consumed until all data is written to force consumer wait (poll)
  uint8_t consumed = 69;
  uint8_t* start_buff = (uint8_t*)buff;
  
  *(uint8_t*)buff = consumed;
  buff += sizeof(uint8_t);
  
  *(uint32_t*)buff = height;
  buff += sizeof(uint32_t);

  strcpy((char*)buff, label);
  buff += (MAX_WORD_LENGTH + 1) * sizeof(char);

  *(uint64_t*)buff = img_sz;
  buff += sizeof(uint64_t);

  memcpy((unsigned char*)buff, img_flat, img_sz);
  buff += img_sz * sizeof(unsigned char);

  // Mark as nonconsumed
  *start_buff = 0;
}

void produce(intptr_t buff, int semid, const char* config_file) {
  
  cv::Ptr<MapTextSynthesizer> mts = MapTextSynthesizer::create(config_file);
  //prepare_synthesis(mts, lexicon_path);

  std::string label;
  cv::Mat image;
  int height;

  int counter = 0;
  while(1) {
    mts->generateSample(label, image, height);

    if(image.data == NULL || label.c_str() == NULL) {
      fprintf(stderr, "Nothing generated by synthesizer.\n");
      exit(1);
    }
    
    /* Update the producer offset */
    lock_buff(semid);
    
    uint64_t image_size = image.rows * image.cols;
    intptr_t write_loc = buff + *((uint64_t*)buff);
    
    // If there's no space to fit this image in the current buff
    if(((*(uint64_t*)buff) + BASE_CHUNK_SIZE + image_size) > SHM_SIZE) {
      //printf("Wrapping!\n");

      // Write in magic number to tell consumer to reset consume offset
      if(((*(uint64_t*)buff) + sizeof(uint64_t)) > SHM_SIZE) {
	//can't fit magic num, need consumer to know not enough space
      } else {
	*((uint64_t*)write_loc) = NO_SPACE_TO_PRODUCE;
      }
      // Reset offset
      *((uint64_t*)buff) = START_BUFF_OFFSET;
      write_loc = buff + *((uint64_t*)buff);
    }
    
    *((uint64_t*)buff) += (BASE_CHUNK_SIZE + image_size);
    //printf("Writing to offset: %ld\n", *((uint64_t*)buff));

    unlock_buff(semid);

    /* Copy data into buff */
    write_data(write_loc, height, label.c_str(),
	       image_size, image.data);
    //printf("Label #%d: %s\n", counter, label.c_str());
  }
}

void cleanup(int signo) {
  /* detach from segment */
  if(shmdt(g_buff) == -1) {
    perror("shmdt");
  }
  exit(1);
}

int main(int argc, char *argv[]) {
  if(argc != 2) {
    fprintf(stderr,"usage: producer \"/path/to/config_file\"");
    exit(1);
  }
  
  struct sigaction sa;
  sa.sa_handler = cleanup;
  sigaction(SIGHUP, NULL, &sa);
  
  g_buff = get_shared_buff(0);
  int semid = get_semaphores(0);
  
  produce((intptr_t)g_buff, semid, argv[1]);
  
  /* detach from segment */
  if(shmdt(g_buff) == -1) {
    perror("shmdt");
    exit(1);
  }
  
  return 0;
}

