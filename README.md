# psd-thumbnails

Windows 탐색기에서 Photoshop **PSD / PSB** 파일의 썸네일을 보여주는 가벼운 셸 확장입니다.
PSD 하나만 담당하고 다른 형식은 건드리지 않습니다.

- 탐색기 밖의 COM 대리 프로세스(`dllhost.exe`)에서 실행되므로 손상된 파일을 만나도 탐색기가 죽지 않습니다.
- 파일 전체를 해석하지 않고 맨 끝의 합성(플랫) 이미지에서 필요한 행만 읽어 축소합니다. 37 MB PSD도 수 ms 안에 끝납니다.
- 관리자 권한 없이 현재 사용자에게만 설치할 수 있습니다.
- 외부 라이브러리 없이 C++17과 Windows SDK(WIC, GDI)만 사용합니다. 결과물은 DLL 하나(약 200 KB)입니다.

## 지원 범위

| 항목 | 지원 |
|---|---|
| 형식 | PSD (version 1), PSB (version 2) |
| 색상 모드 | RGB, Grayscale, CMYK, Lab, Indexed, Bitmap, Duotone, Multichannel |
| 비트 심도 | 1, 8, 16, 32 (float, sRGB로 톤 매핑) |
| 압축 | 무압축, RLE (PackBits) |
| 투명도 | 병합 이미지에 투명도가 있으면 투명 썸네일로 표시 |
| 호환성 최대화 없이 저장된 파일 | 합성 이미지가 없으면 PSD 안의 JPEG 미리보기로 대체, 그것도 없으면 기본 아이콘 |

## 설치

요구 사항: Windows 10 / 11 64비트.

```powershell
.\build.ps1                    # 또는 릴리스의 psdthumb.dll을 build\ 에 둡니다
.\scripts\install.ps1          # 현재 사용자에게 설치 (HKCU, 관리자 권한 불필요)
```

- DLL은 `%LOCALAPPDATA%\psd-thumbnails\psdthumb.dll`로 복사된 뒤 등록됩니다.
- 모든 사용자에게 설치하려면 관리자 PowerShell에서 `.\scripts\install.ps1 -System` 을 실행합니다.
- 탐색기 썸네일 캐시까지 비우려면 `-ClearCache` 를 붙입니다 (탐색기가 재시작됩니다).
- 설치 후 탐색기에서 PSD가 있는 폴더를 열고 보기를 "큰 아이콘" 이상으로 바꿉니다. 이미 열려 있던 폴더는 F5.

제거:

```powershell
.\scripts\install.ps1 -Uninstall            # 사용자 설치 제거
.\scripts\install.ps1 -System -Uninstall    # 시스템 설치 제거 (관리자)
```

스크립트 없이 직접 등록할 수도 있습니다.

```
regsvr32 /n /i:user psdthumb.dll        현재 사용자 등록
regsvr32 /u /n /i:user psdthumb.dll     현재 사용자 해제
regsvr32 psdthumb.dll                   모든 사용자 등록 (관리자)
```

## 빌드

Visual Studio 2022 (또는 Build Tools)에 "C++를 사용한 데스크톱 개발" 워크로드가 필요합니다.

```powershell
.\build.ps1                    # Release -> build\psdthumb.dll, build\psdthumb.exe
.\build.ps1 -Config Debug
```

## 테스트

```powershell
python tests\run_tests.py
```

합성 PSD/PSB 픽스처 27개(색상 모드, 비트 심도, 압축, PSB, 투명도, JPEG 대체, 손상 파일)를 생성해서
`psdthumb.exe`의 출력 색을 확인합니다. 픽스처는 `tests\out\`에 남습니다.

CLI 도구:

```
psdthumb info   file.psd                     헤더, 색상 모드, 투명도, 합성 이미지 유무
psdthumb decode file.psd out.png --size 256  디코더로 직접 썸네일 생성
psdthumb shell  file.psd out.png             셸에 썸네일을 요청 (등록된 핸들러 검증용)
```

## 동작 방식

`IThumbnailProvider` + `IInitializeWithStream`을 구현한 COM 객체 하나입니다. 탐색기가 파일 스트림을 넘겨주면:

1. 헤더 26바이트와 이미지 리소스에서 필요한 값만 읽습니다. 버전 정보(0x0421)의 "실제 병합 데이터" 플래그와 JPEG 미리보기(0x040C)입니다.
2. 레이어 섹션은 레이어 수의 부호만 읽고 건너뜁니다. 부호가 음수면 병합 이미지에 투명도가 있다는 뜻입니다. 16/32비트 문서는 `Lr16`/`Lr32` 블록 안을 봅니다.
3. 파일 끝의 합성 이미지에서 출력 행 수의 약 2배만큼 행을 골라 읽고 박스 필터로 축소합니다. 메모리 사용량은 썸네일 크기에만 비례합니다.
4. sRGB로 변환해 32bpp premultiplied DIB로 돌려줍니다.

## 문제 해결

- **썸네일이 안 보임**: `psdthumb info file.psd`에서 `real merged data: no`이면 Photoshop에서 "호환성 최대화"를 끄고 저장한 파일입니다. 저장 옵션을 "항상"으로 두세요.
- **바뀐 DLL이 반영되지 않음**: `dllhost.exe`가 이전 DLL을 붙잡고 있습니다. `install.ps1`이 자동으로 종료하지만, 수동으로는 작업 관리자에서 COM Surrogate를 종료하면 됩니다.
- **디버그 로그**: 환경 변수 `PSDTHUMB_DEBUG=1`을 설정하면 `%TEMP%\psd-thumbnails.log`에 기록합니다. `dllhost.exe`는 로그인 시점의 환경 변수를 쓰므로 `setx` 후 로그아웃/로그인이 필요할 수 있습니다.

## 제한

- 합성 이미지가 없는 파일은 레이어를 직접 합성하지 않습니다. Photoshop과 같은 결과를 내려면 블렌드 모드와 조정 레이어까지 재현해야 하므로 범위 밖입니다.
- ICC 색 관리를 하지 않습니다. CMYK와 Lab은 단순 수식으로 변환하므로 Photoshop 화면과 채도가 조금 다를 수 있습니다.
- ZIP 압축된 합성 이미지는 지원하지 않습니다. Photoshop은 합성 이미지에 ZIP을 쓰지 않습니다.

## English summary

A minimal Windows Explorer thumbnail provider for Photoshop PSD/PSB files. Single DLL, no dependencies,
runs out-of-process in the COM surrogate so a bad file cannot crash Explorer. Reads only the merged
(composite) image and downsamples it row-wise, so large files stay fast and memory stays small.
Supports RGB / Gray / CMYK / Lab / Indexed / Bitmap / Duotone / Multichannel at 1, 8, 16 and 32 bits,
raw and RLE compression, merged transparency, and falls back to the embedded JPEG preview when the
file was saved without "Maximize Compatibility".

Install for the current user (no admin): `.\build.ps1` then `.\scripts\install.ps1`.
Remove: `.\scripts\install.ps1 -Uninstall`.

## License

MIT
