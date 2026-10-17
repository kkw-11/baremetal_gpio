# STM32F4 Bare-metal GPIO

HAL 라이브러리 없이 **레퍼런스 매뉴얼을 보고 레지스터를 직접 제어**하는 GPIO 예제
파란 USER 버튼을 누르고 있는 동안 보드의 초록 LED ON

> 임베디드 교육을 통한 함께 데이터시트를 보며 작성한 실습 코드
> 수업 이후 주석 정리, 버그 수정, 디버거를 이용한 동작 확인을 직접 진행

## 개발 환경

| 항목 | 내용 |
|---|---|
| 보드 | NUCLEO-F4 시리즈 (STM32F401RE / F411RE) |
| IDE | STM32CubeIDE |
| 디버거 | ST-LINK (보드 내장) |
| 라이브러리 | 사용 안 함 (HAL / CMSIS 미사용) |

## 핀 구성

| 기능 | 핀 | 설정 | 비고 |
|---|---|---|---|
| LED (LD2) | PA5 | 출력 | HIGH = 켜짐 |
| USER 버튼 (B1) | PC13 | 입력, 풀업 | active-low (누르면 0) |

## 코드 구성

```
main.c
├── 1. 주변장치 베이스 주소   RCC, GPIOA~D 주소 (Memory map)
├── 2. GPIO 레지스터 구조체   gpio_regs_t (오프셋 순서대로 매핑)
├── 3. GPIO 드라이버 함수     set_mode / set_pull / write / read / toggle
└── 4. main                   클럭 enable → 핀 설정 → 폴링 루프
```

## 사용한 레지스터

| 레지스터 | 오프셋 | 용도 |
|---|---|---|
| `RCC_AHB1ENR` | RCC + 0x30 | GPIO 포트 클럭 enable (bit0 = A, bit2 = C) |
| `GPIOx_MODER` | 0x00 | 핀 모드 (핀당 2비트: 입력/출력/AF/아날로그) |
| `GPIOx_PUPDR` | 0x0C | 풀업/풀다운 (핀당 2비트) |
| `GPIOx_IDR` | 0x10 | 입력 핀 상태 읽기 |
| `GPIOx_ODR` | 0x14 | 현재 출력 상태 |
| `GPIOx_BSRR` | 0x18 | 비트 set(하위 16) / reset(상위 16), 원자적 출력 |

## HAL과 비교

| 이 코드 | HAL / CMSIS |
|---|---|
| 주소 정의, `gpio_regs_t` | `stm32f4xx.h` → `GPIO_TypeDef` |
| `RCC_AHB1ENR \|= (1 << 0)` | `__HAL_RCC_GPIOA_CLK_ENABLE()` |
| `gpio_set_mode()`, `gpio_set_pull()` | `HAL_GPIO_Init()` |
| `gpio_write()` / `gpio_read()` / `gpio_toggle()` | `HAL_GPIO_WritePin()` / `ReadPin()` / `TogglePin()` |
| 핀 번호 전달 (`5`) | 비트마스크 전달 (`GPIO_PIN_5` = `1 << 5`) |

## 디버거로 확인하기 (STM32CubeIDE)

1. `if(gpio_read(GPIOC, 13) == 0)` 줄에 브레이크포인트 설정
2. 버튼을 누른 상태 / 뗀 상태로 각각 Resume(F8)
3. **SFRs** 탭 → GPIOC → IDR → bit 13 확인 (누름 = 0, 뗌 = 1)
4. 실행 중 실시간 확인: **Live Expressions**에 `*(unsigned int*)0x40020810` 추가

| 관찰 결과 | 의미 |
|---|---|
| 누르면 0, 떼면 1 | 정상 |
| 항상 0 또는 항상 1 | 배선 / 핀 / 클럭 설정 문제 |
| 값이 불규칙하게 바뀜 | 풀업·풀다운 없음 (플로팅) |

## 수정 사항

- **PA13 → PC13 수정**: 버튼 입력 설정이 `GPIOA, 13`으로 되어 있었음.
  PA13은 SWDIO(디버그 핀)이라 설정을 바꾸면 디버거 연결이 끊길 수 있어 `GPIOC, 13`으로 수정
- **시프트 상수 `1`, `3` → `1U`, `3U`**: 핀 15에서 부호 비트를 건드리는 문제 방지
- **클럭 enable 후 더미 읽기 추가**: STM32F4 errata 대응

## 배운 점

- 레퍼런스 매뉴얼에서 메모리 맵과 레지스터 오프셋을 찾아 코드로 옮기는 방법
- HAL 드라이버도 내부적으로는 같은 레지스터를 읽고 쓰는 구조라는 것
- read-modify-write와 BSRR(원자적 쓰기)의 차이
- `volatile`이 하드웨어 레지스터 접근에 필요한 이유
- 디버거로 "버튼 → 핀 → IDR → 코드" 단계별로 문제를 나눠 확인하는 방법

동작 흐름
```
파란 버튼 누름
   ↓
PC13 핀 전압이 HIGH → LOW (보드 회로)
   ↓
GPIOC_IDR bit 13 이 1 → 0 (하드웨어가 자동으로 갱신)
   ↓
gpio_read()가 그 레지스터를 읽음 (내 코드)
   ↓
if 문이 판단 → LED 켜기
```

## 다음 목표

- [ ] 브레드보드에 외부 버튼 연결 (PB0) 후 IDR로 입력 확인
- [ ] 버튼 한 번 누를 때마다 LED 토글 (엣지 검출 + 디바운싱)
- [ ] EXTI 인터럽트 방식으로 변경
- [ ] SysTick으로 정확한 delay 구현
- [X] HAL driver를 사용한 MX_GPIO_Init() 함수 분석(https://github.com/kkw-11/baremetal_gpio/wiki/HAL-driver%EB%A5%BC-%EC%82%AC%EC%9A%A9%ED%95%9C-MX_GPIO_Init()-%ED%95%A8%EC%88%98-%EB%B6%84%EC%84%9D)
