

#include "definitions.h"
#include <device.h>

static void process(uint8_t c);
static void flash_task_(void);
static void update_UpperFlashSerial_(void);
static void trigger_Reset_(void);
static uint32_t get_LowerFlashSerial_(void);
static void update_FlashSerial_(uint32_t serial, uint32_t addr);


typedef enum
{
    BL_CMD_UNLOCK       = 0xa0,
    BL_CMD_DATA         = 0xa1,
    BL_CMD_VERIFY       = 0xa2,
    BL_CMD_RESET        = 0xa3,
    BL_CMD_BKSWAP_RESET = 0xa4,
} APP_STATES;
enum
{
    BL_RESP_OK          = 0x50,
    BL_RESP_ERROR       = 0x51,
    BL_RESP_INVALID     = 0x52,
    BL_RESP_CRC_OK      = 0x53,
    BL_RESP_CRC_FAIL    = 0x54,
};





#define FLASH_START             (0x9d000000UL)
#define FLASH_LENGTH            (0x200000UL)
#define PAGE_SIZE               (2048UL)
#define ERASE_BLOCK_SIZE        (16384UL)
#define PAGES_IN_ERASE_BLOCK    (ERASE_BLOCK_SIZE / PAGE_SIZE)

#define ADDR_OFFSET             0
#define SIZE_OFFSET             1
#define DATA_OFFSET             1
#define CRC_OFFSET              0

#define OFFSET_ALIGN_MASK       (~ERASE_BLOCK_SIZE + 1)
#define SIZE_ALIGN_MASK         (~PAGE_SIZE + 1)



#define CMD_SIZE                1
#define GUARD_SIZE              4
#define SIZE_SIZE               4
#define HEADER_SIZE             (GUARD_SIZE + SIZE_SIZE + CMD_SIZE)
#define DATA_SIZE               ERASE_BLOCK_SIZE
#define WORDS(x)                ((int)((x) / sizeof(uint32_t)))

#define LENGTH_CMD_UNLOCK          17

#define LOWER_FLASH_START               (FLASH_START)
#define LOWER_FLASH_SERIAL_START        (LOWER_FLASH_START + (FLASH_LENGTH / 2) - PAGE_SIZE)
#define LOWER_FLASH_SERIAL_SECTOR       (LOWER_FLASH_START + (FLASH_LENGTH / 2) - ERASE_BLOCK_SIZE)

#define UPPER_FLASH_START               (FLASH_START + (FLASH_LENGTH / 2))
#define UPPER_FLASH_SERIAL_START        (UPPER_FLASH_START + (FLASH_LENGTH / 2) - PAGE_SIZE)
#define UPPER_FLASH_SERIAL_SECTOR       (UPPER_FLASH_START + (FLASH_LENGTH / 2) - ERASE_BLOCK_SIZE)

#define FLASH_SERIAL_CHECKSUM_START     0xDEADBEEF
#define FLASH_SERIAL_CHECKSUM_END       0xBEEFDEAD
#define FLASH_SERIAL_CHECKSUM_CLR       0xFFFFFFFF

#define LOWER_FLASH_SERIAL_READ         ((T_FLASH_SERIAL *)KVA0_TO_KVA1(LOWER_FLASH_SERIAL_START))
#define UPPER_FLASH_SERIAL_READ         ((T_FLASH_SERIAL *)KVA0_TO_KVA1(UPPER_FLASH_SERIAL_START))

static uint32_t CACHE_ALIGN flash_data[WORDS(DATA_SIZE)];
static uint32_t flash_addr          = 0;
static uint32_t unlock_begin        = 0;
static uint32_t unlock_end          = 0;
static uint32_t begin;
static uint32_t end;
static bool         upper_flash_serial_erased   = false;

static uint32_t CACHE_ALIGN input_buffer[4096];
uint8_t         *byte_buf = (uint8_t *)&input_buffer[0];

/* Structure to validate the Flash serial and its checksum
 * Note: The order of the members should not be changed
 */
typedef struct
{
    uint32_t checksum_start;
    uint32_t serial;
    uint32_t checksum_end;
    uint32_t dummy;
} T_FLASH_SERIAL;

T_FLASH_SERIAL CACHE_ALIGN  update_flash_serial_;


APP_STATES state;

uint32_t    count;
uint32_t    count_;
uint8_t     buffer[DATA_SIZE];

void task(void)
{
    uint8_t c = 0;
    count  = 0;
    count_ = 0;
    state = 0xff;
    LED1_On();
    while(1)
    {
        if(UART6_ReceiverIsReady() == true)
        {
            LED1_Toggle();
            c = UART6_ReadByte();
            buffer[count++] = c;
            if( state == 0xff )
            {
                if( count > HEADER_SIZE )
                {

                    byte_buf[count_++] = c;    //input_buffer also updated alongside
                    Nop();
                }
                if( count == LENGTH_CMD_UNLOCK )
                {
                    state = BL_CMD_UNLOCK;
                    process(c);    
                }
            }
            else
            {
                byte_buf[count_++] = c; //input_buffer also updated alongside
                if( count % 64 == 0 )
                {
                    UART6_WriteByte(BL_RESP_OK);
                    LED1_Toggle();

                }
            }
            
        }
        else if(count == DATA_SIZE)
        {
            process(NULL);
        }
        
    }
    
}


static void process(uint8_t c)
{
    
    uint32_t i;
    
    count  = 0;
    count_ = 0;
    switch(state)
    {
        case BL_CMD_UNLOCK:
            begin  = (input_buffer[ADDR_OFFSET] & OFFSET_ALIGN_MASK);

            end    = begin + (input_buffer[SIZE_OFFSET] & SIZE_ALIGN_MASK);

            if (end > begin && end <= (FLASH_START + FLASH_LENGTH))
            {
                unlock_begin = begin;
                unlock_end = end;
                UART6_WriteByte(BL_RESP_OK);
                LED1_On();
                state = BL_CMD_DATA;
                
            }
            else
            {
                unlock_begin = 0;
                unlock_end = 0;
                UART6_WriteByte(BL_RESP_ERROR);
            }
            Nop();
            break;
        case BL_CMD_DATA:
//            flash_addr = (input_buffer[ADDR_OFFSET] & OFFSET_ALIGN_MASK);
            flash_addr = 0x9D100000;

            if (unlock_begin <= flash_addr && flash_addr < unlock_end)
            {
                for (i = 0; i < WORDS(DATA_SIZE); i++)
                    flash_data[i] = input_buffer[i];//input_buffer[i + DATA_OFFSET];

              
                LED2_On();
                flash_task_();
                
                update_UpperFlashSerial_();

//                trigger_Reset_();
            }
            else
            {
                UART6_WriteByte(BL_RESP_ERROR);
            }
            Nop();
            break;
        
        default:
            break;
    }
    
    
}

/* Function to program received application firmware data into internal flash */
static void flash_task_(void)
{
    uint32_t addr       = flash_addr;
    uint32_t page       = 0;
    uint32_t write_idx  = 0;

    if (addr == LOWER_FLASH_SERIAL_SECTOR)
    {
        /* Send error response if active Flash Panels (Lower Flash)
         * Serial Sector is being erased.
         */
//        flash_data_ready = false;

        UART6_WriteByte(BL_RESP_ERROR);
//        LED1_Toggle();
        
        return;
    }
    else if (addr == UPPER_FLASH_SERIAL_SECTOR)
    {
        upper_flash_serial_erased = true;
//        LED2_On();
    }

    /* Erase the Current sector */
    NVM_PageErase(addr);

    /* Wait for erase to complete */
    while(NVM_IsBusy() == true);

    for (page = 0; page < PAGES_IN_ERASE_BLOCK; page++)
    {
        NVM_RowWrite(&flash_data[write_idx], addr);

        while(NVM_IsBusy() == true);

        addr += PAGE_SIZE;
        write_idx += WORDS(PAGE_SIZE);
        LED1_Toggle();
        CORETIMER_DelayMs(30); 
    }

//    flash_data_ready = false;

    UART6_WriteByte(BL_RESP_OK);
}

/* Function to update the serial number in upper flash panel (Inactive Panel) */
static void update_UpperFlashSerial_(void)
{
    uint32_t upper_flash_serial;

    /* Increment Upper Mapped Flash panel serial by 1 to be ahead of the
     * current running Lower Mapped Flash panel serial
     */
    upper_flash_serial = get_LowerFlashSerial_() + 1;

    /* Check if the Serial sector was erased during programming */
    if (upper_flash_serial_erased == false)
    {
        /* Erase the Sector in which Flash Serial Resides */
        NVM_PageErase(UPPER_FLASH_SERIAL_SECTOR);

        /* Wait for erase to complete */
        while(NVM_IsBusy() == true);
    }
    else
    {
        upper_flash_serial_erased = false;
    }

    update_FlashSerial_(upper_flash_serial, UPPER_FLASH_SERIAL_START);
}

/* Function to update the serial number based on address */
static void update_FlashSerial_(uint32_t serial, uint32_t addr)
{
    update_flash_serial_.serial          = serial;
    update_flash_serial_.checksum_start  = FLASH_SERIAL_CHECKSUM_START;
    update_flash_serial_.checksum_end    = FLASH_SERIAL_CHECKSUM_END;

    NVM_QuadWordWrite((uint32_t *)&update_flash_serial_, addr);

    while(NVM_IsBusy() == true);
}

/* Function to read the Serial number from Flash bank mapped to lower region */
static uint32_t get_LowerFlashSerial_(void)
{
    T_FLASH_SERIAL *lower_flash_serial = LOWER_FLASH_SERIAL_READ;

    return (lower_flash_serial->serial);
}

/* Function to Send the final response for reset command and trigger a reset */
static void trigger_Reset_(void)
{
    UART6_WriteByte(BL_RESP_OK);

    while(UART6_TransmitComplete() == false);

    /* Perform system unlock sequence */ 
    SYSKEY = 0x00000000;
    SYSKEY = 0xAA996655;
    SYSKEY = 0x556699AA;

    RSWRSTSET = _RSWRST_SWRST_MASK;
    (void)RSWRST;
}
