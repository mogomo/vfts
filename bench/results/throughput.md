| Engine | enron | pubmed | msmarco | wiki_he | wiki_ar |
|---|---:|---:|---:|---:|---:|
| vtext, tserve (one statement per session) | 5,785 (5,782-6,396) | 9,891 (9,791-10,039) | 3,227 (3,227-3,233) | 20,526 (20,350-20,720) | 17,588 (17,466-17,827) |
| vtext, batches (8 sessions, idle-else-queue) | 2,344 (2,340-2,424) | 3,024 (3,004-3,029) | 1,812 (1,810-1,820) | 3,607 (3,597-3,613) | 3,473 (3,461-3,477) |
| vtext, one statement per query (32 sessions) | 1,166 (1,162-1,170) | 1,241 (1,226-1,245) | 934 (922-935) | 1,308 (1,306-1,309) | 1,327 (1,316-1,328) |
| Elasticsearch | 3,902 (3,684-4,044) | 7,964 (7,813-8,339) | 2,729 (2,581-2,798) | 10,100 (10,057-10,181) | 10,050 (10,013-10,134) |
| OpenSearch | 3,881 (3,782-3,918) | 8,122 (8,075-8,290) | 2,907 (2,732-2,991) | 10,166 (10,041-10,305) | 10,469 (10,455-10,528) |

One client: mean ms per query (best of 3 runs per query), median over rounds; p99 in brackets

| Engine | enron | pubmed | msmarco | wiki_he | wiki_ar |
|---|---:|---:|---:|---:|---:|
| vtext, tserve (one statement per session) | 2.12 (20.7) | 1.19 (5.7) | 2.94 (13.8) | 0.55 (3.1) | 0.63 (4.9) |
| vtext, one statement per query (32 sessions) | 3.39 (7.7) | 3.18 (4.2) | 3.83 (5.9) | 3.12 (5.0) | 3.12 (5.6) |
| Elasticsearch | 2.75 (31.4) | 1.30 (5.3) | 3.95 (16.2) | 1.09 (4.5) | 1.07 (4.2) |
| OpenSearch | 2.40 (28.7) | 1.07 (2.8) | 3.60 (16.0) | 1.06 (4.1) | 1.09 (4.3) |

vtext serve / best of Elasticsearch and OpenSearch (median q/s):
- enron: 1.48x
- pubmed: 1.22x
- msmarco: 1.11x
- wiki_he: 2.02x
- wiki_ar: 1.68x
