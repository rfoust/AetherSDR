#include "core/backends/flex/VitaRxReorder.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
int main(int argc,char**argv){if(argc!=2)return 2;QFile f(QString::fromLocal8Bit(argv[1]));if(!f.open(QIODevice::ReadOnly)||f.size()>2000000)return 2;const auto doc=QJsonDocument::fromJson(f.readAll());int failures=0;for(auto row:doc.array()){const auto o=row.toObject();AetherSDR::VitaRxReorder b;int old=0,last=-1,lost=0,breaks=0,reordered=0;for(auto v:o["points"].toArray()){const auto a=v.toArray();int seq=a[0].toInt();if(last>=0&&seq!=((last+1)&15))++old;last=seq;auto r=b.add(seq,{quint32(a[2].toDouble()),quint64(a[3].toDouble()),true},qint64(a[1].toDouble()));lost+=r.lost;breaks+=r.breaks;reordered+=r.reordered;}auto r=b.expire(1000000);lost+=r.lost;breaks+=r.breaks;printf("pcc=0x%04x received=%lld before_errors=%d after_missing=%d after_gaps=%d reordered=%d\n",o["pcc"].toInt(),(long long)o["points"].toArray().size(),old,lost,breaks,reordered);if(lost||breaks)++failures;}return failures?1:0;}
