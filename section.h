#ifndef SECTION_H
#define SECTION_H

#define STRIPED_SECTION __not_in_flash("Striped")
#define DMA_SECTION __not_in_flash("DMA")
#define SCAN_OUT_INNER_SECTION __not_in_flash("ScanOutInner")
#define SCAN_OUT_DATA_SECTION __scratch_y("ScanOutData")

#endif  // SECTION_H
