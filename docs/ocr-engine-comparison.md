# 글자 영역 탐지와 인식 엔진 비교

현재 배포본의 Tesseract 언어/배치 조정만으로 실제 만화 인식률이 충분하지 않아,
번역 프로그램처럼 탐지와 인식을 분리하는 방식을 비교한다. 이 도구는 DB,
원본 파일, 기존 설치본을 수정하지 않는다. 비교 결과만으로 앱 기본 엔진을 바꾸지 않는다.

## 비교 대상

- `tesseract-page`: 같은 페이지를 정밀 Tesseract의 PSM 11/6으로 읽고 인식 신뢰도가
  높은 결과 선택. 언어를 지정한 참고 기준이다. 앱의 자동 언어/배치 점수와 완전히
  동일한 실행 경로는 아니다.
- `detector-tesseract`: PP-OCRv5 server 탐지 결과를 영역별로 잘라 기존 엔진으로 읽기.
- `detector-paddle`: 동일한 영역을 일본어 PP-OCRv5 server / 한국어 전용 mobile로 읽기.
- `detector-manga`: 동일한 일본어 영역을 Manga OCR로 읽기. 한국어에는 실행하지 않는다.

탐지는 동일한 입력 이미지에서 한 번만 수행한다. 정답 제목/작가/영역은 엔진 선택이나
OCR 입력에 제공하지 않는다. 현재 사각형 crop과 위→아래 / 세로열 오른쪽→왼쪽 정렬은
초기 비교용이며, 기울어진 글자의 원근 보정이나 복잡한 말풍선 읽기 순서를 해결하지 않는다.

## 측정 및 한계

`report.json`에 전체 OCR 전사, 각 식별 필드의 문자 오류, 정확한 복구 여부,
영역 수, 처리 시간과 패키지 버전을 기록한다. NFKC와 공백만 정규화한다.
문자 오류는 정답 문자열과 OCR 전사에서 가장 가까운 부분 문자열의 편집 거리다.
OCR에 이름이 포함됐다는 것과 그 이름을 작가로 올바르게 분류했다는 것은 다르다.
제목·작가의 역할 연결 및 안전한 DB 입력은 별도로 검증해야 한다.

- 언어는 시험 자료에 명시한다. 자동 언어 판별 성능을 측정하지 않는다.
- 신뢰도를 엔진 간 정확도 확률로 비교하지 않는다.
- 모델 최초 준비 시간과 페이지 처리 시간을 분리한다.
- 영역 방식의 시간에는 탐지 시간이 포함된다. 프로세스 재시작 시간은 포함되지 않는다.
- 공개 CI에는 직접 생성한 판권/패널/작은 글자/흰 글자와 빈 페이지를 사용한다.
  실제 작품, 사용자 스크린샷, 제목, 작가, 연락처를 커밋하거나 CI 입력으로 보내지 않는다.
- 합성 시험에서 잘 읽힌다고 실제 만화의 인식률이 보장되는 것은 아니다.
- Manga OCR은 글자 없는 이미지에도 그럴듯한 문장을 만들 수 있어, 탐지 없이 페이지
  전체에 실행하거나 그 결과를 자동 확정하면 안 된다.

## 실행

Python 3.11, CPU를 기준으로 `.github/workflows/ocr-comparison.yml`에서 실행한다.
패키지는 `tools/ocr_benchmark/requirements.txt`에 고정하며, 전이 의존성은 실행 결과의
`ocr-packages.txt`에 기록한다. 현재 모델 배포 파일은 제공자의 모델 저장소에서 가져오므로
패키지 버전 고정이 모델 바이트까지 고정한다는 뜻은 아니다. 제품 배포 전에는 모델 파일을
해시 고정하고 오프라인 실행 및 재배포 조건을 확인해야 한다.

```sh
python -m pip install -r tools/ocr_benchmark/requirements.txt
python -m unittest discover -s tools/ocr_benchmark -p test_core.py
python tools/ocr_benchmark/fixtures.py --font /path/to/NotoSansCJKjp-Regular.otf --output fixtures
python tools/ocr_benchmark/compare.py --manifest fixtures/manifest.json --output comparison --tesseract /path/to/tesseract --tessdata /path/to/tessdata_best
```

처음 실행할 때 모델 파일을 내려받는다. OCR 이미지를 외부 서비스에 업로드하지 않는다.
완전한 오프라인 제품 패키지로 배포된 도구는 아직 아니다.

`manifest.json`은 `synthetic`과 `cases`를 담는다. 각 case는 `id`, 상대 `image` 경로,
`language` (`jpn`/`kor`), 실제 `title`, `author`, 참고 `boxes`를 갖는다.
실제 페이지는 공개 CI에 올리지 않고 로컬에서만 평가한다. 개인정보가 있는 판권은
별도 검토가 필요하다. 폴더와 압축파일은 앱의 공통 `readPages()`에서 얻은 같은 페이지
이미지를 사용할 수 있으며, 이 비교 스크립트 자체는 압축작품 등록 기능을 대신하지 않는다.

## 적용 판단

먼저 탐지 누락과 인식 오류를 분리해 확인하고, 여러 실제 작품에서 식별 필드 복구율과
CPU 시간을 비교한다. 선택한 엔진은 취소 가능한 별도 프로세스로 연결하고, 모델을
설치본에 포함한 뒤 인터넷/개발 도구 없는 Windows와 한글 경로에서 검증한다.
폴더·압축 공통 경로, 읽던 위치, 기존 메타데이터 보존은 기존 회귀 검사를 유지한다.

참고한 원본 구현:
- https://github.com/zyddnys/manga-image-translator
- https://github.com/dmMaze/comic-text-detector
- https://github.com/kha-white/manga-ocr
- https://github.com/PaddlePaddle/PaddleOCR/tree/v3.2.0

## 2026-09-07 비교 결과와 시험 적용

Windows CPU 실행 `34134188887` (server 탐지), `34135058558` (mobile 탐지) 성공.
생성한 8장, 제목·작가 16개 항목을 대상으로 Tesseract page 11/16,
탐지+Tesseract 12/16, 탐지+PaddleOCR 16/16, 일본어 탐지+Manga OCR 8/8이었다.
빈 페이지는 모든 방식에서 텍스트를 만들지 않았다. Mobile 탐지는 참고 글자 영역
32개를 모두 포함했고, 탐지+PaddleOCR의 페이지당 시간은 약 1.65~3.25초였다.
Server 탐지+PaddleOCR은 약 15.15~17.82초였다. 모델 준비/프로세스 시작은 제외한다.
상세 전사는 `ocr-comparison-mobile.json`에 보존한다. 실제 작품의 정확도 수치가 아니다.

한국어·일본어를 함께 제공하고 추가 일본어 모델 크기를 줄이기 위해 우선
mobile 탐지 + PaddleOCR 일본어/한국어 인식을 앱의 선택 가능한 시험 모드로 연결한다.
Manga OCR은 이 시험에서 일본어 정확도 차이를 입증하지 못해 설치본에는 넣지 않는다.
기본 Tesseract 모드는 유지한다. 시험 모드에서는 인식 결과의 후보를 직접 선택하며
자동 입력/자동 외부 검색을 실행하지 않는다. 명시적인 검색·저장은 기존 흐름을 사용한다.

`tools/neural_ocr/models.lock.json`은 비교 실행에서 확인한 모델 9개 파일의 SHA-256을
고정한다. 빌드 시 다운로드 바이트를 대조하고 CPython·의존성과 함께 별도 하위 폴더에
배포한다. 워커는 로컬 모델만 열고 Python 소켓 연결을 차단한다. 이 선택 모드의
설치 후 검증은 Windows 워크플로에서 별도로 수행한다.

시험 모드를 포함한 `10.3.0.26`은 Windows 실행 `34135749563`에서 설치 후 검증까지
통과했다. `tools/neural_ocr/constraints.txt`는 이 설치본에서 실제 사용한 의존 버전이다.
비교 도구는 Manga OCR 때문에 별도 Transformers/PyTorch 의존성을 사용하므로,
비교 환경과 제품 워커의 전체 패키지 목록이 같지는 않다. 설치본 워커에는 Manga OCR,
PyTorch, Transformers를 포함하지 않는다.
