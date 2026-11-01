#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include "tempo_clock.h"

#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,m); exit(1); } } while (0)
#define NEAR(a,b,m) CHECK(fabs((a)-(b)) < 2e-8,m)

static double tick(UniTempoClock *c, double *beat, bool have, bool playing,
                   double host, double custom, bool useCustom,
                   double speed, double cycle, double sr) {
    double p=uni_tempo_step(c,*beat,have,playing,host,custom,useCustom,speed,cycle,sr);
    if(playing)*beat+=host/(60*sr);
    return p;
}

int main(void) {
    const double sr=48000;
    UniTempoClock c;
    const double tempos[]={60,90,120,180};
    for(unsigned t=0;t<4;t++) {
        uni_tempo_reset(&c);double beat=0,bpm=tempos[t];
        NEAR(tick(&c,&beat,true,true,bpm,120,false,1,4,sr),0,"phase starts before advancing");
        for(int i=1;i<24000;i++)tick(&c,&beat,true,true,bpm,120,false,1,4,sr);
        NEAR(tick(&c,&beat,true,true,bpm,120,false,1,4,sr),bpm/(60*8),"host BPM determines phase");
        uni_tempo_reset(&c);beat=0;
        for(int i=0;i<24000;i++)tick(&c,&beat,false,true,bpm,120,false,1,4,sr);
        NEAR(tick(&c,&beat,false,true,bpm,120,false,1,4,sr),bpm/(60*8),"missing beat integrates actual BPM");
    }
    printf("PASS HOST at 60/90/120/180 BPM with timeline and tempo-only fallback\n");

    const double speeds[]={.5,1,2};
    for(unsigned s=0;s<3;s++) {
        uni_tempo_reset(&c);double beat=0;
        for(int i=0;i<24000;i++)tick(&c,&beat,true,true,120,90,false,speeds[s],4,sr);
        NEAR(tick(&c,&beat,true,true,120,90,false,speeds[s],4,sr),.25*speeds[s],"half/double host speed");
        uni_tempo_reset(&c);beat=0;
        for(int i=0;i<24000;i++)tick(&c,&beat,true,true,180,90,true,speeds[s],4,sr);
        NEAR(tick(&c,&beat,true,true,180,90,true,speeds[s],4,sr),.1875*speeds[s],"custom BPM ignores host tempo");
    }
    printf("PASS half/normal/double speed and CUSTOM 90 BPM against HOST 180 BPM\n");

    /* A four-bar 3/4 cycle is twelve quarter notes, versus sixteen in 4/4. */
    uni_tempo_reset(&c);double beat=6;
    NEAR(tick(&c,&beat,true,true,120,120,false,1,12,sr),.5,"3/4 four-bar midpoint");
    uni_tempo_reset(&c);beat=6;
    NEAR(tick(&c,&beat,true,true,120,120,false,1,16,sr),.375,"4/4 four-bar phase");
    uni_tempo_reset(&c);beat=-2;
    NEAR(tick(&c,&beat,true,true,120,120,false,1,12,sr),10.0/12,"negative host position wraps");
    printf("PASS meter-dependent cycle lengths and negative timeline positions\n");

    /* Rate edits preserve the exact phase while changing only future rate. */
    uni_tempo_reset(&c);beat=2.5;
    tick(&c,&beat,true,true,120,90,false,1,4,sr);
    double before=c.phase;
    NEAR(tick(&c,&beat,true,true,120,90,false,2,8,sr),before,"host speed/cycle edits continuous");
    before=c.phase;
    NEAR(tick(&c,&beat,true,true,120,90,true,2,8,sr),before,"entering custom preserves phase");
    before=c.phase;
    NEAR(tick(&c,&beat,true,true,180,150,true,.5,3,sr),before,"custom BPM/speed/cycle edits continuous");
    before=c.phase;
    NEAR(tick(&c,&beat,true,true,180,150,true,.5,3,sr),before,"host tempo edit does not reset custom phase");
    double expected_phase=uni_tempo_wrap((beat/3)*.5);
    NEAR(tick(&c,&beat,true,true,180,150,false,.5,3,sr),expected_phase,"entering HOST aligns to song position");
    printf("PASS continuous rate edits and HOST/CUSTOM mode entry semantics\n");

    uni_tempo_reset(&c);beat=1;
    tick(&c,&beat,true,true,120,90,false,1,4,sr);
    before=c.phase;
    NEAR(tick(&c,&beat,true,false,120,90,false,1,4,sr),before,"stop freezes host phase");
    for(int i=0;i<10;i++)NEAR(tick(&c,&beat,true,false,120,90,false,1,4,sr),before,"stopped phase stays frozen");
    beat=7;
    NEAR(tick(&c,&beat,true,false,120,90,false,1,4,sr),.75,"stopped HOST seek rebases");
    NEAR(tick(&c,&beat,true,true,120,90,false,1,4,sr),.75,"HOST restart follows current song position");
    beat=0;
    NEAR(tick(&c,&beat,true,true,120,90,false,1,4,sr),0,"HOST loop jump rebases");

    uni_tempo_reset(&c);beat=1;
    tick(&c,&beat,true,true,120,90,true,1,4,sr);
    before=c.phase;
    NEAR(tick(&c,&beat,true,false,120,90,true,1,4,sr),before,"stop freezes custom phase");
    beat=8;
    NEAR(tick(&c,&beat,true,false,120,90,true,1,4,sr),0,"custom seek resets to zero");
    NEAR(tick(&c,&beat,true,true,120,90,true,1,4,sr),0,"custom restart resets to zero");
    for(int i=0;i<24000;i++)tick(&c,&beat,true,true,120,90,true,1,4,sr);
    CHECK(c.phase>.18,"custom clock advanced");beat=0;
    NEAR(tick(&c,&beat,true,true,120,90,true,1,4,sr),0,"custom loop jump resets");
    printf("PASS stop, seek, restart and loop handling in both modes\n");

    /* Missing timeline acquisition and modest timestamp jitter. */
    uni_tempo_reset(&c);beat=20;
    for(int i=0;i<24;i++)tick(&c,&beat,false,true,120,90,false,1,4,sr);
    expected_phase=uni_tempo_wrap(beat/4);
    NEAR(tick(&c,&beat,true,true,120,90,false,1,4,sr),expected_phase,"HOST acquires late timeline");
    uni_tempo_reset(&c);beat=20;
    for(int i=0;i<24;i++)tick(&c,&beat,false,true,120,90,true,1,4,sr);
    before=c.phase;
    NEAR(tick(&c,&beat,true,true,120,90,true,1,4,sr),before,"CUSTOM timeline acquisition preserves phase");
    before=c.phase;beat+=2*(120/(60*sr));
    NEAR(tick(&c,&beat,true,true,120,90,true,1,4,sr),before,"two-sample timeline jitter is tolerated");
    printf("PASS late timeline acquisition and timestamp rounding tolerance\n");

    uni_tempo_reset(&c);beat=0;
    for(int i=0;i<100;i++)NEAR(tick(&c,&beat,true,true,120,90,false,0,4,sr),0,"zero speed pauses phase");
    double p=uni_tempo_step(&c,NAN,true,true,NAN,INFINITY,true,NAN,0,-1);
    CHECK(isfinite(p)&&p>=0&&p<1,"invalid values produce finite phase");
    p=uni_tempo_step(&c,DBL_MAX,true,true,DBL_MAX,DBL_MAX,false,DBL_MAX,DBL_MIN,DBL_MIN);
    CHECK(isfinite(p)&&p>=0&&p<1,"extreme values produce finite phase");
    CHECK(uni_tempo_step(NULL,0,true,true,120,120,false,1,4,sr)==0,"null clock is safe");
    printf("PASS zero speed and invalid/extreme finite guards\nALL TEMPO CLOCK TESTS PASSED\n");
    return 0;
}
