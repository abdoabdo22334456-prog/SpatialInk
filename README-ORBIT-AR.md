# Spatial Ink Orbit 0.2.0

نسخة جديدة باسم **Spatial Ink Orbit** فيها سلايدر **ORBIT SIZE** لتوسيع وتصغير دايرة الأوربت. بتتركّب **جنب** Spatial Ink الأصلي من غير ما تلمسه.

## التركيب

1. انسخ `SpatialInkOrbit.clap` إلى `C:\Program Files\Common Files\CLAP` (نفس مكان النسخة القديمة).
2. في FL Studio: `Options > File settings > Manage plugins` ثم `Find installed plugins` مع تفعيل `Verify plugins`.
3. هتلاقي **Spatial Ink Orbit** في القائمة كبلجن منفصل. النسخة القديمة `SpatialInk.clap` تفضل زي ما هي.

## ORBIT SIZE

- السلايدر تحت REACH مباشرة. المدى من 5% لـ 95% من نصف قطر الخريطة، والافتراضي 50% (نفس حجم الأوربت في النسخة القديمة).
- اضغط **ORBIT** الأول، وبعدها اسحب السلايدر: الدايرة بتكبر وتصغر وانت بتسمع، والصوت بيبعد أو يقرب معاها.
- ينفع تعمل له Automation من FL Studio، ويتحفظ مع المشروع.
- لو رسمت مسار بإيدك أو استخدمت PLACE أو FAR > EAR أو WHISPER أو CLEAR، المسار ده مبيتأثرش بالسلايدر. اضغط ORBIT تاني للرجوع للأوربت.
- المسافة الفعلية = حجم الدايرة × REACH. يعني REACH بيحدد أقصى مسافة، والسلايدر بيحدد الدايرة جواها.

## ملاحظات

- الـ ID اتغيّر إلى `local.spatialink.orbit`، فمشاريع Spatial Ink القديمة مش هتفتح في النسخة دي. اعمل Add للبلجن الجديد وابدأ من جديد.
- باقي الإمكانيات (DRAW, PLACE, UNDO, FAR > EAR, WHISPER, CLEAR, Follow/Manual) زي ما هي، وشرحها في `README-AR.md`.
- **لم يتم تشغيله داخل FL Studio نفسه.** اتجرّب في مضيف CLAP تجريبي (انظر `TEST-REPORT-ORBIT.md`).

## إعادة البناء

`source/build-orbit.ps1 -LlvmMingwRoot <مسار LLVM-MinGW>` ينتج `SpatialInkOrbit.clap`. الكود في `source/spatial_ink_orbit.c`.
