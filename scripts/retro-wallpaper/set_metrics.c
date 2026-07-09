/* set_metrics.exe - shrink desktop icon spacing to 70x72 px live via
 * SPI_SETICONMETRICS, so snap-to-grid (if stuck on) snaps to the same grid the
 * arranger uses instead of the wider ~78px default. */
#include <windows.h>
int main(void){
    ICONMETRICS im; im.cbSize = sizeof(im);
    if(!SystemParametersInfo(SPI_GETICONMETRICS, sizeof(im), &im, 0)) return 1;
    im.iHorzSpacing = 70;
    im.iVertSpacing = 72;
    SystemParametersInfo(SPI_SETICONMETRICS, sizeof(im), &im,
                         SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    return 0;
}
