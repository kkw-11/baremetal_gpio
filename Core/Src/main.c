/**
 * STM32F4 (Nucleo-F401RE/F411RE) 베어메탈 GPIO 예제
 *  - HAL/CMSIS 없이 레퍼런스 매뉴얼의 메모리 맵을 보고 레지스터를 직접 제어
 *  - 동작: 파란 USER 버튼(B1, PC13)을 누르고 있는 동안 초록 LED(LD2, PA5) 켜짐
 */

#include <stdint.h>
#include <stdbool.h>

/* ========================================================================
 * 1. 주변장치 베이스 주소 (Reference Manual - Memory map)
 *    GPIO와 RCC는 AHB1 버스에 연결되어 있음
 * ======================================================================== */
#define RCC_BASE        0x40023800UL    // RCC: 클럭 제어
#define GPIOA_BASE      0x40020000UL    // 각 GPIO 포트는 0x400 간격
#define GPIOB_BASE      0x40020400UL
#define GPIOC_BASE      0x40020800UL
#define GPIOD_BASE      0x40020C00UL

/* 주소를 32비트 레지스터처럼 읽고 쓰게 해주는 매크로
 * volatile: 컴파일러가 최적화로 읽기/쓰기를 생략하지 못하게 함
 *           (하드웨어가 값을 바꾸는 레지스터이므로 매번 실제로 접근해야 함) */
#define REG(addr)       (*(volatile uint32_t *)(addr))

/* RCC_AHB1ENR: AHB1 버스에 붙은 주변장치의 클럭 enable 레지스터 (오프셋 0x30)
 *   bit0 = GPIOAEN, bit1 = GPIOBEN, bit2 = GPIOCEN ... */
#define RCC_AHB1ENR     REG(RCC_BASE + 0x30)

/* (참고) 개별 레지스터 매크로 방식
 * 아래 구조체 방식으로 바꾼 뒤로는 사용하지 않음 */
#define GPIOA_MODER     REG(GPIOA_BASE + 0x00)
#define GPIOA_OTYPER    REG(GPIOA_BASE + 0x04)
#define GPIOA_OSPEEDR   REG(GPIOA_BASE + 0x08)
#define GPIOA_PUPDR     REG(GPIOA_BASE + 0x0C)
#define GPIOA_ODR       REG(GPIOA_BASE + 0x14)

#define GPIOC_MODER     REG(GPIOC_BASE + 0x00)
#define GPIOC_PUPDR     REG(GPIOC_BASE + 0x0C)
#define GPIOC_IDR       REG(GPIOC_BASE + 0x10)

/* ========================================================================
 * 2. GPIO 레지스터 구조체
 *    멤버 순서 = 레지스터 오프셋 순서 (각 4바이트씩 증가)
 *    포트 베이스 주소에 이 구조체를 겹쳐 놓으면 port->IDR 처럼 접근 가능
 * ======================================================================== */
typedef struct{
	volatile uint32_t MODER;    // 0x00 모드: 00=입력, 01=출력, 10=AF, 11=아날로그 (핀당 2비트)
	volatile uint32_t OTYPER;   // 0x04 출력 타입: 0=푸시풀, 1=오픈드레인 (핀당 1비트)
	volatile uint32_t OSPEEDR;  // 0x08 출력 속도 (핀당 2비트)
	volatile uint32_t PUPDR;    // 0x0C 풀업/풀다운: 00=없음, 01=풀업, 10=풀다운 (핀당 2비트)
	volatile uint32_t IDR;      // 0x10 입력 데이터: 핀의 실제 전압 상태 (읽기 전용)
	volatile uint32_t ODR;      // 0x14 출력 데이터: 현재 출력 상태
	volatile uint32_t BSRR;     // 0x18 비트 set/reset: 하위 16비트=set, 상위 16비트=reset (쓰기 전용)
	volatile uint32_t LCKR;     // 0x1C 설정 잠금
	volatile uint32_t AFRL;     // 0x20 대체 기능 선택 (핀 0~7)
	volatile uint32_t AFRH;     // 0x24 대체 기능 선택 (핀 8~15)
} gpio_regs_t;

/* 베이스 주소를 구조체 포인터로 캐스팅 → GPIOA->MODER 형태로 사용 */
#define GPIOA ((gpio_regs_t *) GPIOA_BASE)
#define GPIOB ((gpio_regs_t *) GPIOB_BASE)
#define GPIOC ((gpio_regs_t *) GPIOC_BASE)
#define GPIOD ((gpio_regs_t *) GPIOD_BASE)

/* MODER 레지스터 값과 1:1로 대응 (0, 1, 2, 3) */
typedef enum{
	GPIO_MODE_IN,       // 00 입력
	GPIO_MODE_OUT,      // 01 범용 출력
	GPIO_MODE_AF,       // 10 대체 기능 (UART, SPI 등)
	GPIO_MODE_ANALOG    // 11 아날로그 (ADC 등)
} gpio_mode_t;

/* PUPDR 레지스터 값과 1:1로 대응 (0, 1, 2) */
typedef enum{
	GPIO_PULL_NONE,     // 00 풀업/풀다운 없음
	GPIO_PULL_UP,       // 01 내부 풀업
	GPIO_PULL_DOWN      // 10 내부 풀다운
} gpio_pull_t;

/* ========================================================================
 * 3. GPIO 드라이버 함수
 * ======================================================================== */

/* 단순 반복 지연 (정확한 시간 아님, 클럭/최적화 수준에 따라 달라짐)
 * n을 volatile로 선언해 컴파일러가 빈 루프를 지우지 못하게 함 */
void delay(volatile uint32_t n){
	while(n--);
}

/* 핀 모드 설정 (MODER, 핀당 2비트)
 * read-modify-write: 다른 핀 설정은 유지하고 해당 핀의 2비트만 변경 */
void gpio_set_mode(gpio_regs_t *port, uint16_t pin, gpio_mode_t mode){
	uint32_t v = port->MODER;               // 1) 현재 값 읽기

	v &= ~(3U << (pin*2));                  // 2) 해당 핀의 2비트를 00으로 지우기
	v |= ((uint32_t)mode << (pin*2));       // 3) 원하는 모드 값 넣기

	port->MODER = v;                        // 4) 다시 쓰기
}

/* 풀업/풀다운 설정 (PUPDR, 핀당 2비트), 방식은 gpio_set_mode와 동일 */
void gpio_set_pull(gpio_regs_t *port, uint16_t pin, gpio_pull_t pull){
	uint32_t v = port->PUPDR;
	v &= ~(3U << (pin*2));                  // 해당 핀 2비트 지우기
	v |= ((uint32_t) pull << (pin *2));     // 풀 설정 값 넣기
	port->PUPDR = v;
}

/* 핀 출력 (BSRR 사용)
 *  - level=1 → 하위 비트(pin)에 1 쓰기  → 해당 핀 HIGH
 *  - level=0 → 상위 비트(pin+16)에 1 쓰기 → 해당 핀 LOW
 * 한 번의 쓰기로 끝나서 다른 핀에 영향 없음 (원자적 동작) */
void gpio_write(gpio_regs_t *port, uint16_t pin, bool level){
	port->BSRR = level ? (1U << pin) : (1U << (pin + 16));
}

/* 핀 입력 읽기: IDR을 pin만큼 오른쪽 시프트 후 최하위 비트만 남김 → 0 또는 1 */
bool gpio_read(const gpio_regs_t *port, uint16_t pin){
	return (port->IDR >> pin) & 1;
}

/* 핀 토글: 현재 출력(ODR)을 보고 반대로 바꿈
 *  - 현재 1이면 → (odr & mask) << 16  : reset 비트 → LOW
 *  - 현재 0이면 → (~odr & mask)       : set 비트   → HIGH
 * 결과를 BSRR에 한 번에 씀 */
void gpio_toggle(gpio_regs_t *port, uint16_t pin){
	uint32_t mask = 1U << pin;
	uint32_t odr = port->ODR;
	port->BSRR = ((odr & mask) << 16) | (~odr & mask);
}

/* ========================================================================
 * 4. 메인
 * ======================================================================== */
int main(void)
{
	/* 1) GPIO 포트 클럭 켜기
	 *    클럭이 꺼져 있으면 GPIO 레지스터에 써도 반영되지 않음 */
	RCC_AHB1ENR |= (1U << 0);     // GPIOA 클럭 enable (LED: PA5)
	RCC_AHB1ENR |= (1U << 2);     // GPIOC 클럭 enable (버튼: PC13)
	(void)RCC_AHB1ENR;            // 클럭 enable 후 짧은 지연 (F4 errata 대응, 더미 읽기)

	/* 2) 핀 설정 */
	gpio_set_mode(GPIOA, 5, GPIO_MODE_OUT);    // PA5 = LD2 LED → 출력

	/* [수정] 원래 코드: GPIOA 13
	 *  PA13은 SWDIO(디버거 연결 핀)라서 바꾸면 디버거 연결이 끊길 수 있음
	 *  버튼은 PC13이므로 GPIOC로 수정 */
	gpio_set_mode(GPIOC, 13, GPIO_MODE_IN);    // PC13 = B1 버튼 → 입력
	gpio_set_pull(GPIOC, 13, GPIO_PULL_UP);    // 풀업 (Nucleo는 외부 풀업이 있어 생략 가능)

	/* 3) 메인 루프 (폴링 방식: 계속 버튼 상태를 확인) */
	while(1)
	{
		/* 버튼은 active-low: 누르면 0, 떼면 1 */
		if(gpio_read(GPIOC, 13) == 0){
			gpio_write(GPIOA, 5, 1);    // 눌림 → LED ON
		}else{
			gpio_write(GPIOA, 5, 0);    // 안 눌림 → LED OFF
		}
	}

	return 0;   // 무한 루프라 실제로 도달하지 않음
}
