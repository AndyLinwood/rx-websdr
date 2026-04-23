var nbands=8;
var ini_freq=7100.000000;
var ini_mode='lsb';
var chseq=2;
var bandinfo= [
  { centerfreq: 29100.000000,
    samplerate: 768.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 29110.000000,
    maxzoom: 5,
    name: '10mH',
    scaleimgs: [
      ["tmp/1776709222-b0z0i0.png"],
      ["tmp/1776709222-b0z1i0.png","tmp/1776709222-b0z1i1.png"],
      ["tmp/1776709222-b0z2i0.png","tmp/1776709222-b0z2i1.png","tmp/1776709222-b0z2i2.png","tmp/1776709222-b0z2i3.png"],
      ["tmp/1776709222-b0z3i0.png","tmp/1776709222-b0z3i1.png","tmp/1776709222-b0z3i2.png","tmp/1776709222-b0z3i3.png","tmp/1776709222-b0z3i4.png","tmp/1776709222-b0z3i5.png","tmp/1776709222-b0z3i6.png","tmp/1776709222-b0z3i7.png"],
      ["tmp/1776709222-b0z4i0.png","tmp/1776709222-b0z4i1.png","tmp/1776709222-b0z4i2.png","tmp/1776709222-b0z4i3.png","tmp/1776709222-b0z4i4.png","tmp/1776709222-b0z4i5.png","tmp/1776709222-b0z4i6.png","tmp/1776709222-b0z4i7.png","tmp/1776709222-b0z4i8.png","tmp/1776709222-b0z4i9.png","tmp/1776709222-b0z4i10.png","tmp/1776709222-b0z4i11.png","tmp/1776709222-b0z4i12.png","tmp/1776709222-b0z4i13.png","tmp/1776709222-b0z4i14.png","tmp/1776709222-b0z4i15.png"],
      ["tmp/1776709222-b0z5i0.png","tmp/1776709222-b0z5i1.png","tmp/1776709222-b0z5i2.png","tmp/1776709222-b0z5i3.png","tmp/1776709222-b0z5i4.png","tmp/1776709222-b0z5i5.png","tmp/1776709222-b0z5i6.png","tmp/1776709222-b0z5i7.png","tmp/1776709222-b0z5i8.png","tmp/1776709222-b0z5i9.png","tmp/1776709222-b0z5i10.png","tmp/1776709222-b0z5i11.png","tmp/1776709222-b0z5i12.png","tmp/1776709222-b0z5i13.png","tmp/1776709222-b0z5i14.png","tmp/1776709222-b0z5i15.png","tmp/1776709222-b0z5i16.png","tmp/1776709222-b0z5i17.png","tmp/1776709222-b0z5i18.png","tmp/1776709222-b0z5i19.png","tmp/1776709222-b0z5i20.png","tmp/1776709222-b0z5i21.png","tmp/1776709222-b0z5i22.png","tmp/1776709222-b0z5i23.png","tmp/1776709222-b0z5i24.png","tmp/1776709222-b0z5i25.png","tmp/1776709222-b0z5i26.png","tmp/1776709222-b0z5i27.png","tmp/1776709222-b0z5i28.png","tmp/1776709222-b0z5i29.png","tmp/1776709222-b0z5i30.png","tmp/1776709222-b0z5i31.png"]]
  }
,  { centerfreq: 28350.000000,
    samplerate: 768.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 28360.000000,
    maxzoom: 5,
    name: '10mL',
    scaleimgs: [
      ["tmp/1776709222-b1z0i0.png"],
      ["tmp/1776709222-b1z1i0.png","tmp/1776709222-b1z1i1.png"],
      ["tmp/1776709222-b1z2i0.png","tmp/1776709222-b1z2i1.png","tmp/1776709222-b1z2i2.png","tmp/1776709222-b1z2i3.png"],
      ["tmp/1776709222-b1z3i0.png","tmp/1776709222-b1z3i1.png","tmp/1776709222-b1z3i2.png","tmp/1776709222-b1z3i3.png","tmp/1776709222-b1z3i4.png","tmp/1776709222-b1z3i5.png","tmp/1776709222-b1z3i6.png","tmp/1776709222-b1z3i7.png"],
      ["tmp/1776709222-b1z4i0.png","tmp/1776709222-b1z4i1.png","tmp/1776709222-b1z4i2.png","tmp/1776709222-b1z4i3.png","tmp/1776709222-b1z4i4.png","tmp/1776709222-b1z4i5.png","tmp/1776709222-b1z4i6.png","tmp/1776709222-b1z4i7.png","tmp/1776709222-b1z4i8.png","tmp/1776709222-b1z4i9.png","tmp/1776709222-b1z4i10.png","tmp/1776709222-b1z4i11.png","tmp/1776709222-b1z4i12.png","tmp/1776709222-b1z4i13.png","tmp/1776709222-b1z4i14.png","tmp/1776709222-b1z4i15.png"],
      ["tmp/1776709222-b1z5i0.png","tmp/1776709222-b1z5i1.png","tmp/1776709222-b1z5i2.png","tmp/1776709222-b1z5i3.png","tmp/1776709222-b1z5i4.png","tmp/1776709222-b1z5i5.png","tmp/1776709222-b1z5i6.png","tmp/1776709222-b1z5i7.png","tmp/1776709222-b1z5i8.png","tmp/1776709222-b1z5i9.png","tmp/1776709222-b1z5i10.png","tmp/1776709222-b1z5i11.png","tmp/1776709222-b1z5i12.png","tmp/1776709222-b1z5i13.png","tmp/1776709222-b1z5i14.png","tmp/1776709222-b1z5i15.png","tmp/1776709222-b1z5i16.png","tmp/1776709222-b1z5i17.png","tmp/1776709222-b1z5i18.png","tmp/1776709222-b1z5i19.png","tmp/1776709222-b1z5i20.png","tmp/1776709222-b1z5i21.png","tmp/1776709222-b1z5i22.png","tmp/1776709222-b1z5i23.png","tmp/1776709222-b1z5i24.png","tmp/1776709222-b1z5i25.png","tmp/1776709222-b1z5i26.png","tmp/1776709222-b1z5i27.png","tmp/1776709222-b1z5i28.png","tmp/1776709222-b1z5i29.png","tmp/1776709222-b1z5i30.png","tmp/1776709222-b1z5i31.png"]]
  }
,  { centerfreq: 27345.000000,
    samplerate: 768.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 27355.000000,
    maxzoom: 5,
    name: '11m',
    scaleimgs: [
      ["tmp/1776709222-b2z0i0.png"],
      ["tmp/1776709222-b2z1i0.png","tmp/1776709222-b2z1i1.png"],
      ["tmp/1776709222-b2z2i0.png","tmp/1776709222-b2z2i1.png","tmp/1776709222-b2z2i2.png","tmp/1776709222-b2z2i3.png"],
      ["tmp/1776709222-b2z3i0.png","tmp/1776709222-b2z3i1.png","tmp/1776709222-b2z3i2.png","tmp/1776709222-b2z3i3.png","tmp/1776709222-b2z3i4.png","tmp/1776709222-b2z3i5.png","tmp/1776709222-b2z3i6.png","tmp/1776709222-b2z3i7.png"],
      ["tmp/1776709222-b2z4i0.png","tmp/1776709222-b2z4i1.png","tmp/1776709222-b2z4i2.png","tmp/1776709222-b2z4i3.png","tmp/1776709222-b2z4i4.png","tmp/1776709222-b2z4i5.png","tmp/1776709222-b2z4i6.png","tmp/1776709222-b2z4i7.png","tmp/1776709222-b2z4i8.png","tmp/1776709222-b2z4i9.png","tmp/1776709222-b2z4i10.png","tmp/1776709222-b2z4i11.png","tmp/1776709222-b2z4i12.png","tmp/1776709222-b2z4i13.png","tmp/1776709222-b2z4i14.png","tmp/1776709222-b2z4i15.png"],
      ["tmp/1776709222-b2z5i0.png","tmp/1776709222-b2z5i1.png","tmp/1776709222-b2z5i2.png","tmp/1776709222-b2z5i3.png","tmp/1776709222-b2z5i4.png","tmp/1776709222-b2z5i5.png","tmp/1776709222-b2z5i6.png","tmp/1776709222-b2z5i7.png","tmp/1776709222-b2z5i8.png","tmp/1776709222-b2z5i9.png","tmp/1776709222-b2z5i10.png","tmp/1776709222-b2z5i11.png","tmp/1776709222-b2z5i12.png","tmp/1776709222-b2z5i13.png","tmp/1776709222-b2z5i14.png","tmp/1776709222-b2z5i15.png","tmp/1776709222-b2z5i16.png","tmp/1776709222-b2z5i17.png","tmp/1776709222-b2z5i18.png","tmp/1776709222-b2z5i19.png","tmp/1776709222-b2z5i20.png","tmp/1776709222-b2z5i21.png","tmp/1776709222-b2z5i22.png","tmp/1776709222-b2z5i23.png","tmp/1776709222-b2z5i24.png","tmp/1776709222-b2z5i25.png","tmp/1776709222-b2z5i26.png","tmp/1776709222-b2z5i27.png","tmp/1776709222-b2z5i28.png","tmp/1776709222-b2z5i29.png","tmp/1776709222-b2z5i30.png","tmp/1776709222-b2z5i31.png"]]
  }
,  { centerfreq: 21225.000000,
    samplerate: 768.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 21235.000000,
    maxzoom: 5,
    name: '15m',
    scaleimgs: [
      ["tmp/1776709222-b3z0i0.png"],
      ["tmp/1776709222-b3z1i0.png","tmp/1776709222-b3z1i1.png"],
      ["tmp/1776709222-b3z2i0.png","tmp/1776709222-b3z2i1.png","tmp/1776709222-b3z2i2.png","tmp/1776709222-b3z2i3.png"],
      ["tmp/1776709222-b3z3i0.png","tmp/1776709222-b3z3i1.png","tmp/1776709222-b3z3i2.png","tmp/1776709222-b3z3i3.png","tmp/1776709222-b3z3i4.png","tmp/1776709222-b3z3i5.png","tmp/1776709222-b3z3i6.png","tmp/1776709222-b3z3i7.png"],
      ["tmp/1776709222-b3z4i0.png","tmp/1776709222-b3z4i1.png","tmp/1776709222-b3z4i2.png","tmp/1776709222-b3z4i3.png","tmp/1776709222-b3z4i4.png","tmp/1776709222-b3z4i5.png","tmp/1776709222-b3z4i6.png","tmp/1776709222-b3z4i7.png","tmp/1776709222-b3z4i8.png","tmp/1776709222-b3z4i9.png","tmp/1776709222-b3z4i10.png","tmp/1776709222-b3z4i11.png","tmp/1776709222-b3z4i12.png","tmp/1776709222-b3z4i13.png","tmp/1776709222-b3z4i14.png","tmp/1776709222-b3z4i15.png"],
      ["tmp/1776709222-b3z5i0.png","tmp/1776709222-b3z5i1.png","tmp/1776709222-b3z5i2.png","tmp/1776709222-b3z5i3.png","tmp/1776709222-b3z5i4.png","tmp/1776709222-b3z5i5.png","tmp/1776709222-b3z5i6.png","tmp/1776709222-b3z5i7.png","tmp/1776709222-b3z5i8.png","tmp/1776709222-b3z5i9.png","tmp/1776709222-b3z5i10.png","tmp/1776709222-b3z5i11.png","tmp/1776709222-b3z5i12.png","tmp/1776709222-b3z5i13.png","tmp/1776709222-b3z5i14.png","tmp/1776709222-b3z5i15.png","tmp/1776709222-b3z5i16.png","tmp/1776709222-b3z5i17.png","tmp/1776709222-b3z5i18.png","tmp/1776709222-b3z5i19.png","tmp/1776709222-b3z5i20.png","tmp/1776709222-b3z5i21.png","tmp/1776709222-b3z5i22.png","tmp/1776709222-b3z5i23.png","tmp/1776709222-b3z5i24.png","tmp/1776709222-b3z5i25.png","tmp/1776709222-b3z5i26.png","tmp/1776709222-b3z5i27.png","tmp/1776709222-b3z5i28.png","tmp/1776709222-b3z5i29.png","tmp/1776709222-b3z5i30.png","tmp/1776709222-b3z5i31.png"]]
  }
,  { centerfreq: 14175.000000,
    samplerate: 384.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 14185.000000,
    maxzoom: 4,
    name: '20m',
    scaleimgs: [
      ["tmp/1776709222-b4z0i0.png"],
      ["tmp/1776709222-b4z1i0.png","tmp/1776709222-b4z1i1.png"],
      ["tmp/1776709222-b4z2i0.png","tmp/1776709222-b4z2i1.png","tmp/1776709222-b4z2i2.png","tmp/1776709222-b4z2i3.png"],
      ["tmp/1776709222-b4z3i0.png","tmp/1776709222-b4z3i1.png","tmp/1776709222-b4z3i2.png","tmp/1776709222-b4z3i3.png","tmp/1776709222-b4z3i4.png","tmp/1776709222-b4z3i5.png","tmp/1776709222-b4z3i6.png","tmp/1776709222-b4z3i7.png"],
      ["tmp/1776709222-b4z4i0.png","tmp/1776709222-b4z4i1.png","tmp/1776709222-b4z4i2.png","tmp/1776709222-b4z4i3.png","tmp/1776709222-b4z4i4.png","tmp/1776709222-b4z4i5.png","tmp/1776709222-b4z4i6.png","tmp/1776709222-b4z4i7.png","tmp/1776709222-b4z4i8.png","tmp/1776709222-b4z4i9.png","tmp/1776709222-b4z4i10.png","tmp/1776709222-b4z4i11.png","tmp/1776709222-b4z4i12.png","tmp/1776709222-b4z4i13.png","tmp/1776709222-b4z4i14.png","tmp/1776709222-b4z4i15.png"]]
  }
,  { centerfreq: 7100.000000,
    samplerate: 384.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 7110.000000,
    maxzoom: 4,
    name: '40m',
    scaleimgs: [
      ["tmp/1776709222-b5z0i0.png"],
      ["tmp/1776709222-b5z1i0.png","tmp/1776709222-b5z1i1.png"],
      ["tmp/1776709222-b5z2i0.png","tmp/1776709222-b5z2i1.png","tmp/1776709222-b5z2i2.png","tmp/1776709222-b5z2i3.png"],
      ["tmp/1776709222-b5z3i0.png","tmp/1776709222-b5z3i1.png","tmp/1776709222-b5z3i2.png","tmp/1776709222-b5z3i3.png","tmp/1776709222-b5z3i4.png","tmp/1776709222-b5z3i5.png","tmp/1776709222-b5z3i6.png","tmp/1776709222-b5z3i7.png"],
      ["tmp/1776709222-b5z4i0.png","tmp/1776709222-b5z4i1.png","tmp/1776709222-b5z4i2.png","tmp/1776709222-b5z4i3.png","tmp/1776709222-b5z4i4.png","tmp/1776709222-b5z4i5.png","tmp/1776709222-b5z4i6.png","tmp/1776709222-b5z4i7.png","tmp/1776709222-b5z4i8.png","tmp/1776709222-b5z4i9.png","tmp/1776709222-b5z4i10.png","tmp/1776709222-b5z4i11.png","tmp/1776709222-b5z4i12.png","tmp/1776709222-b5z4i13.png","tmp/1776709222-b5z4i14.png","tmp/1776709222-b5z4i15.png"]]
  }
,  { centerfreq: 3660.000000,
    samplerate: 384.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 3670.000000,
    maxzoom: 4,
    name: '80m',
    scaleimgs: [
      ["tmp/1776709222-b6z0i0.png"],
      ["tmp/1776709222-b6z1i0.png","tmp/1776709222-b6z1i1.png"],
      ["tmp/1776709222-b6z2i0.png","tmp/1776709222-b6z2i1.png","tmp/1776709222-b6z2i2.png","tmp/1776709222-b6z2i3.png"],
      ["tmp/1776709222-b6z3i0.png","tmp/1776709222-b6z3i1.png","tmp/1776709222-b6z3i2.png","tmp/1776709222-b6z3i3.png","tmp/1776709222-b6z3i4.png","tmp/1776709222-b6z3i5.png","tmp/1776709222-b6z3i6.png","tmp/1776709222-b6z3i7.png"],
      ["tmp/1776709222-b6z4i0.png","tmp/1776709222-b6z4i1.png","tmp/1776709222-b6z4i2.png","tmp/1776709222-b6z4i3.png","tmp/1776709222-b6z4i4.png","tmp/1776709222-b6z4i5.png","tmp/1776709222-b6z4i6.png","tmp/1776709222-b6z4i7.png","tmp/1776709222-b6z4i8.png","tmp/1776709222-b6z4i9.png","tmp/1776709222-b6z4i10.png","tmp/1776709222-b6z4i11.png","tmp/1776709222-b6z4i12.png","tmp/1776709222-b6z4i13.png","tmp/1776709222-b6z4i14.png","tmp/1776709222-b6z4i15.png"]]
  }
,  { centerfreq: 1895.000000,
    samplerate: 192.000000,
    tuningstep: 0.031250,
    maxlinbw: 8.000000,
    vfo: 1905.000000,
    maxzoom: 3,
    name: '160m',
    scaleimgs: [
      ["tmp/1776709222-b7z0i0.png"],
      ["tmp/1776709222-b7z1i0.png","tmp/1776709222-b7z1i1.png"],
      ["tmp/1776709222-b7z2i0.png","tmp/1776709222-b7z2i1.png","tmp/1776709222-b7z2i2.png","tmp/1776709222-b7z2i3.png"],
      ["tmp/1776709222-b7z3i0.png","tmp/1776709222-b7z3i1.png","tmp/1776709222-b7z3i2.png","tmp/1776709222-b7z3i3.png","tmp/1776709222-b7z3i4.png","tmp/1776709222-b7z3i5.png","tmp/1776709222-b7z3i6.png","tmp/1776709222-b7z3i7.png"]]
  }
];
var dxinfoavailable=1;
var idletimeout=900000000;
var has_mobile=1;
