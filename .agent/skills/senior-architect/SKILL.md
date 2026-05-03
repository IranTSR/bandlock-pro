---
name: senior-architect
description: Software architecture guidance for designing scalable systems. Covers architecture patterns, tech stack decisions, system design trade-offs, and integration patterns.
---

Note: Current UI is Next.js on Vercel: https://{{FRONTEND_URL}} (production app) and https://{{STAGING_FRONTEND_URL}} (staging).


# Senior Architect

Guidance for architecture decisions in {{PROJECT_NAME}} and similar projects.

## When to Use

- System architecture decisions (monolith vs microservices, sync vs async)
- Tech stack selection and trade-offs
- Database design and data modeling
- API design patterns (REST, GraphQL, event-driven)
- Scalability and performance architecture
- Integration patterns between services

## Architecture Decision Framework

### 1. Understand Requirements

```
Functional Requirements    → What the system must do
Non-Functional Requirements → How well it must do it
├── Performance (latency, throughput)
├── Scalability (users, data volume)
├── Availability (uptime, redundancy)
├── Security (auth, encryption)
└── Maintainability (team size, deployment frequency)
```

### 2. Evaluate Trade-offs

| Decision | Option A | Option B | Choose A When | Choose B When |
|----------|----------|----------|---------------|---------------|
| Sync vs Async | REST API | Message Queue | Low latency needed | Decoupling, reliability |
| Monolith vs Microservices | Single deploy | Many services | Small team, early stage | Scale teams independently |
| SQL vs NoSQL | PostgreSQL | MongoDB/Redis | Relations, ACID | Flexible schema, speed |
| Server vs Serverless | Railway/EC2 | Lambda/Functions | Predictable load, state | Spiky load, stateless |

### 3. Document Decisions (ADR Format)

```markdown
# ADR-001: Use PostgreSQL for Campaign Data

## Status: Accepted

## Context
Campaign data has complex relationships (offers → lists → segments).
Need ACID for financial calculations (EPC, revenue).

## Decision
Use PostgreSQL with proper indexing for analytics queries.

## Consequences
+ Strong consistency for revenue calculations
+ Complex joins for analytics
- Need to manage connection pooling
- Schema migrations require planning
```

## Common Patterns

### API Layer (FastAPI)

```
Request → Middleware → Router → Service → Repository → Database
                ↓
           Validation (Pydantic)
                ↓
           Response
```

### Data Pipeline (Batch)

```
Source (Google Sheets) → Extract → Transform → Load → PostgreSQL
                              ↓
                         Validate
                              ↓
                         Log metrics
```

### AI Agent Architecture

```
User Query → Orchestrator Agent
                 ↓
         Tool Selection
         ├── SQL Tool (structured data)
         ├── Memory Tool (patterns, history)
         └── Search Tool (web, docs)
                 ↓
         Analyst Agent (reasoning)
                 ↓
         Judge Agent (validation)
                 ↓
         Response
```

## {{PROJECT_NAME}} Tech Stack

| Layer | Technology | Why |
|-------|------------|-----|
| Frontend | Next.js (Vercel) | Production app UI with fast iteration |
| Backend | FastAPI | Async, Pydantic, OpenAPI docs |
| Database | PostgreSQL | ACID, complex analytics queries |
| Memory | ChromaDB | Semantic search for patterns |
| LLM | Claude (Anthropic) | Best reasoning, tool use |
| Deploy | Railway | Simple, auto-deploy from git |

## Anti-Patterns to Avoid

1. **Premature optimization** - Profile first, optimize second
2. **Over-engineering** - Start simple, add complexity when needed
3. **Distributed transactions** - Use saga pattern or avoid
4. **Shared mutable state** - Use message passing instead
5. **God classes** - Split by responsibility

## Quick Reference

```bash
# Check system dependencies
python -c "import sys; print(sys.version)"
pip list | grep -E "fastapi|sqlalchemy|chromadb"

# Database schema
alembic heads  # Check migration state
alembic upgrade head  # Apply migrations

# Service health
curl http://localhost:8000/health
```
