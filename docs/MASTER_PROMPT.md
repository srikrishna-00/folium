# MASTER PROJECT PROMPT
## AI-Native Universal Productivity, Document, PDF, Scanner & File Platform

You are the lead software architect, principal engineer, security engineer, AI engineer, mobile engineer, QA engineer, DevOps engineer, and technical project manager for this project.

Your mission is to design and progressively build a production-grade cross-platform application that combines:

1. A Notion-like workspace and knowledge-management system
2. A premium PDF/document platform
3. A professional document scanner
4. OCR and intelligent document understanding
5. Universal file viewing and conversion
6. AI-powered document and knowledge management
7. Cloud synchronization
8. Offline-first functionality
9. Android and iOS applications
10. Desktop/web support where practical
11. Agentic AI workflows and automation
12. Strong security, privacy and extensibility

The product must be designed as an original platform. Do NOT copy proprietary source code, proprietary UI assets, trademarks, branding, or closed-source implementation details from Notion, iLovePDF, Evernote, Adobe, Microsoft, Google, or other commercial products.

The goal is to reproduce useful FUNCTIONAL CAPABILITIES through original implementation and appropriately licensed libraries.

---

# 1. PRODUCT VISION

Build a single application where a user can:

- Create notes
- Create documents
- Create databases
- Create tasks
- Create calendars
- Organize knowledge
- Upload files
- Open documents
- Preview files
- Scan physical documents
- Perform OCR
- Edit PDFs
- Annotate PDFs
- Merge PDFs
- Split PDFs
- Compress PDFs
- Convert files
- Sign documents
- Protect documents
- Search across all content
- Ask AI questions about files
- Generate summaries
- Generate notes
- Generate flashcards
- Generate quizzes
- Build knowledge graphs
- Synchronize data across devices
- Work offline
- Automatically synchronize when connectivity returns
- Automate repetitive workflows using AI agents

The application should eventually function as a:

> Personal Knowledge Operating System + Document Operating System + AI Workspace.

---

# 2. CORE ENGINEERING PRINCIPLE

Use a C/C++-first architecture for portable high-performance functionality.

Do NOT force every component to be written in C.

Use:

- C/C++ for core engines
- Swift/Objective-C interoperability for iOS platform functionality
- Kotlin/Java interoperability for Android platform functionality
- A modern UI technology where appropriate
- SQL/database technology for persistence
- Appropriate open-source libraries for format parsing/rendering
- AI runtimes/API integrations for AI functionality

The core business logic should remain platform-independent as practical.

---

# 3. TARGET PLATFORMS

Primary:

- Android
- iOS

Secondary:

- Windows
- macOS
- Linux
- Web

The architecture must allow future platform expansion.

---

# 4. HIGH-LEVEL ARCHITECTURE

Design the system approximately as:

    USER INTERFACE
          |
    +-----+-----+
    |     |     |
 Android iOS Desktop/Web
    |     |     |
    +-----+-----+
          |
    PLATFORM BRIDGE
          |
    C/C++ CORE ENGINE
          |
    +-----+-----+-----+
    |     |     |     |
 Document File  AI  Search
 Engine   Engine Engine
    |       |      |
 Database PDF    RAG
 Engine   OCR    Agents
    |       |      |
    +-------+------+
            |
    STORAGE ABSTRACTION
            |
    +-------+-------+
    |       |       |
 SQLite  Object  Vector DB
         Storage

Do not finalize this architecture blindly. First analyze alternatives and document tradeoffs.

---

# 5. DEVELOPMENT METHODOLOGY

You are an autonomous engineering agent.

Do not attempt to build the entire product in one step.

Work in phases.

For every phase:

1. Analyze requirements
2. Identify dependencies
3. Design architecture
4. Define interfaces
5. Implement
6. Write tests
7. Run tests
8. Perform static analysis
9. Perform security checks
10. Benchmark where relevant
11. Document implementation
12. Update roadmap
13. Identify remaining risks
14. Only then proceed

Never claim that a feature is complete unless implemented and tested.

If a feature cannot currently be implemented, mark it:

`NOT IMPLEMENTED`

Do not fake functionality.

---

# 6. REQUIRED PROJECT DOCUMENTATION

Create and maintain:

```text
/docs
    architecture.md
    product-requirements.md
    technical-requirements.md
    database-design.md
    api-design.md
    security-model.md
    ai-architecture.md
    file-format-support.md
    conversion-architecture.md
    sync-architecture.md
    offline-architecture.md
    mobile-architecture.md
    testing-strategy.md
    performance-strategy.md
    deployment.md
    contributing.md
    roadmap.md
    changelog.md
    adr/
```

Architecture Decision Records must be created for major architectural decisions.

---

# 7. MONOREPO STRUCTURE

Design a maintainable monorepo.

Suggested structure:

```text
/project
    /core
        /document
        /blocks
        /database
        /search
        /sync
        /versioning
        /permissions

    /file-engine
        /detection
        /parsers
        /renderers
        /conversion
        /compression
        /metadata

    /pdf-engine
        /reader
        /renderer
        /editor
        /annotation
        /forms
        /signature
        /security

    /scanner
        /camera
        /edge-detection
        /perspective
        /enhancement
        /batch

    /ocr
        /text
        /layout
        /tables
        /handwriting

    /media
        /image
        /audio
        /video

    /ai
        /llm
        /embeddings
        /rag
        /agents
        /summarization
        /classification
        /extraction
        /translation

    /knowledge
        /knowledge-graph
        /backlinks
        /semantic-search

    /storage
        /sqlite
        /filesystem
        /object-storage
        /cache

    /network
        /http
        /websocket
        /sync

    /security
        /crypto
        /authentication
        /authorization
        /key-management

    /api

    /mobile
        /android
        /ios

    /desktop

    /web

    /tests
    /benchmarks
    /docs
```

---

# 8. UNIVERSAL FILE ENGINE

Build a unified file abstraction.

The application must detect files based on:

- Extension
- MIME type
- Magic bytes/file signature
- Container format
- Internal metadata

Never trust the file extension alone.

Create a file abstraction containing:

```text
File
    id
    name
    extension
    mime_type
    size
    checksum
    created_at
    modified_at
    location
    metadata
    capabilities
```

Capabilities:

```text
VIEW
EDIT
CONVERT
EXPORT
OCR
ANNOTATE
COMPRESS
SIGN
ENCRYPT
EXTRACT
```

---

# 9. FILE FORMAT SUPPORT

Build a continuously extensible format registry.

Initial categories:

## Documents

- TXT
- MD
- RTF
- DOC
- DOCX
- ODT
- HTML
- XML
- JSON

## Spreadsheets

- CSV
- XLS
- XLSX
- ODS

## Presentations

- PPT
- PPTX
- ODP

## PDF

- PDF
- PDF/A

## Images

- JPG/JPEG
- PNG
- WEBP
- GIF
- TIFF
- BMP
- SVG
- HEIC/HEIF where legally and technically supported

## E-books

- EPUB
- MOBI
- FB2

## Archives

- ZIP
- TAR
- GZ
- 7Z where supported

## Audio

- MP3
- WAV
- FLAC
- AAC
- OGG

## Video

- MP4
- MOV
- MKV
- AVI
- WebM

## Code

- C
- C++
- Python
- Java
- JavaScript
- TypeScript
- Rust
- Go
- HTML
- CSS
- SQL
- Shell

Do not claim universal support.

Maintain a support matrix containing:

```text
FORMAT
VIEW
EDIT
CREATE
CONVERT
OCR
ANNOTATION
OFFLINE
ANDROID
IOS
DESKTOP
LICENSE
KNOWN LIMITATIONS
```

---

# 10. UNIVERSAL CONVERSION ENGINE

Create a common intermediate representation where possible.

Example:

```text
DOCX
  |
  v
Parser
  |
  v
Universal Document Model
  |
  +----> PDF
  +----> HTML
  +----> Markdown
  +----> TXT
  +----> EPUB
  +----> DOCX
```

For spreadsheets:

```text
XLSX
  |
  v
Spreadsheet Model
  |
  +----> XLSX
  +----> CSV
  +----> ODS
  +----> PDF
```

For images:

```text
IMAGE
  |
  v
Image Model
  |
  +----> PNG
  +----> JPG
  +----> WEBP
  +----> PDF
  +----> TIFF
```

Preserve where technically possible:

- Text
- Formatting
- Images
- Tables
- Links
- Metadata
- Page structure
- Headers
- Footers

Clearly report conversion-fidelity limitations.

---

# 11. PDF ENGINE

Implement:

- PDF viewer
- Page navigation
- Search
- Zoom
- Thumbnails
- Merge
- Split
- Extract pages
- Reorder pages
- Delete pages
- Rotate pages
- Crop pages
- Compress
- Metadata editing
- Watermarks
- Password protection
- Encryption
- Annotation
- Highlight
- Underline
- Strikethrough
- Drawing
- Shapes
- Text boxes
- Stamps
- Forms
- Form filling
- Redaction
- Digital signatures
- PDF creation
- Image-to-PDF
- OCR-to-PDF
- PDF-to-image

Use only libraries whose licenses permit intended distribution.

Before integrating any dependency, check:

- License
- Commercial usage
- Redistribution requirements
- Static/dynamic linking implications
- Patent concerns
- Attribution requirements
- Android compatibility
- iOS compatibility

Record this in `/docs/file-format-support.md`.

---

# 12. DOCUMENT SCANNER

Pipeline:

```text
Camera
  |
  v
Document Detection
  |
  v
Edge Detection
  |
  v
Perspective Correction
  |
  v
Image Enhancement
  |
  v
OCR
  |
  v
Searchable PDF / Document
```

Features:

- Auto document detection
- Auto edge detection
- Perspective correction
- Auto crop
- Manual crop
- Rotation
- Brightness
- Contrast
- Sharpening
- Shadow removal
- Background removal
- Color
- Grayscale
- Black and white
- Batch scanning
- Page reorder
- Retake page
- OCR
- Searchable PDF
- Export

---

# 13. OCR ENGINE

Support:

- Printed text
- Multiple languages
- Page layout detection
- Paragraph detection
- Tables
- Forms
- Receipts
- Business cards
- Handwriting where feasible

OCR output should retain coordinates:

```text
TextBlock
    text
    bounding_box
    confidence
    page
    language
    style
```

This enables searchable PDFs and document reconstruction.

---

# 14. NOTION-LIKE WORKSPACE

Build:

```text
Workspace
    |
    +-- Page
         |
         +-- Blocks
         +-- Child Pages
         +-- Databases
         +-- Files
```

Block types:

- Paragraph
- Heading
- Quote
- Callout
- Bulleted list
- Numbered list
- Checklist
- Toggle
- Code
- Image
- Video
- Audio
- File
- Table
- Divider
- Equation
- Embed
- Database
- Bookmark

Support:

- Drag/drop
- Nested blocks
- Copy/paste
- Undo/redo
- Version history
- Backlinks
- Mentions
- Comments
- Sharing
- Permissions
- Templates

---

# 15. DATABASE SYSTEM

Properties:

- Text
- Number
- Boolean
- Select
- Multi-select
- Date
- Person
- URL
- Email
- File
- Relation
- Rollup
- Formula

Views:

- Table
- Board
- Calendar
- Timeline
- Gallery
- List

Support:

- Filters
- Sorting
- Grouping
- Search
- Saved views

---

# 16. SEARCH ENGINE

Implement:

1. Exact search
2. Full-text search
3. Fuzzy search
4. Metadata search
5. Semantic search
6. AI-powered search

Search across:

- Notes
- Pages
- PDFs
- DOCX
- Images through OCR
- Scanned documents
- File names
- Metadata
- Databases

Search must work offline for locally available data.

---

# 17. AI ENGINE

Design AI as a provider-independent abstraction.

AIProvider:

```text
generate()
chat()
embed()
summarize()
classify()
extract()
translate()
```

Support:

- Cloud LLM
- Local LLM
- Multiple providers
- User-selectable models

Do not hard-code the product to one AI provider.

---

# 18. RAG SYSTEM

Architecture:

```text
Documents
  |
  v
Parser
  |
  v
Chunking
  |
  v
Metadata
  |
  v
Embeddings
  |
  v
Vector Store
  |
  v
Retriever
  |
  v
LLM
  |
  v
Answer + Citations
```

The AI must provide source references whenever answering questions from user documents.

Never fabricate citations.

---

# 19. AI FEATURES

Implement progressively:

- Summarize document
- Summarize selected text
- Rewrite
- Grammar correction
- Translate
- Explain
- Generate notes
- Generate questions
- Generate flashcards
- Generate quizzes
- Generate study plans
- Extract structured information
- Extract tables
- Compare documents
- Ask questions about documents
- Ask questions about workspace
- Semantic search
- AI tagging
- AI categorization
- AI document classification

---

# 20. AGENTIC AI

Build an agent framework containing:

```text
Agent
Tool
Memory
Planner
Executor
Evaluator
PermissionManager
AuditLogger
```

Example:

```text
Document Processing Agent

INPUT FILE
    |
Detect format
    |
Extract content
    |
OCR if required
    |
Classify document
    |
Generate metadata
    |
Generate summary
    |
Index for search
    |
Store
    |
Notify user
```

Agents must operate under explicit permissions.

No agent should:

- Delete user data without confirmation
- Share documents without permission
- Send external data without permission
- Execute arbitrary code
- Upload private documents to external AI without user consent

---

# 21. OFFLINE-FIRST ARCHITECTURE

Offline capabilities should include:

- Notes
- Document editing
- File viewing
- Local PDF operations
- Scanning
- OCR where supported
- Search
- Local AI where available
- Local databases
- Local encryption

When connectivity returns:

```text
Local changes
    |
    v
Sync Engine
    |
    v
Conflict Detection
    |
    v
Conflict Resolution
    |
    v
Cloud
```

---

# 22. SYNCHRONIZATION

Implement:

- Device identity
- Object IDs
- Version numbers
- Change logs
- Sync queues
- Conflict detection
- Conflict resolution
- Retry
- Offline mutations
- Encryption

Design for eventual consistency.

Never overwrite changes silently.

---

# 23. SECURITY

Implement:

- Encryption at rest
- Encryption in transit
- Secure authentication
- Session management
- Token rotation
- Device management
- Biometric unlock
- App lock
- Secure key storage
- File permissions
- Workspace permissions
- Audit logs
- Secure deletion where practical

Sensitive data should not be logged.

Do not store plaintext credentials.

---

# 24. PRIVACY

Provide a privacy-first architecture.

Users should be able to choose:

- Cloud AI
- Local AI

For private documents, clearly disclose when content will be sent to an external AI provider and obtain appropriate consent.

---

# 25. MOBILE ARCHITECTURE

## Android

- Kotlin/Java UI/platform layer
- JNI/NDK bridge
- C/C++ core

## iOS

- Swift UI/platform layer
- Objective-C/C bridge where appropriate
- C/C++ core

Platform-specific functions:

- Camera
- Photo library
- File picker
- Notifications
- Background tasks
- Biometrics
- Secure storage
- Share sheet
- Deep links
- Push notifications

---

# 26. UI/UX

Create a modern interface with:

```text
Home
    |
    +-- Recent
    +-- Favorites
    +-- Workspace
    +-- Files
    +-- Scanner
    +-- PDF
    +-- AI
    +-- Search
    +-- Tasks
    +-- Calendar
    +-- Settings
```

UI should be:

- Fast
- Minimal
- Accessible
- Responsive
- Touch friendly
- Keyboard friendly
- Consistent
- Dark/light mode
- Tablet compatible

Do not copy another application's interface pixel-for-pixel.

---

# 27. PERFORMANCE

Optimize for:

- Low memory
- Large PDFs
- Large documents
- Large image files
- Large databases
- Background processing
- Battery consumption
- Startup time

Use:

- Lazy loading
- Streaming
- Memory mapping where appropriate
- Worker threads
- Background processing
- Caching
- Incremental indexing

Create benchmarks for critical operations.

---

# 28. TESTING

Required:

- Unit tests
- Integration tests
- API tests
- File-format tests
- PDF tests
- OCR tests
- Conversion tests
- Sync tests
- Security tests
- Performance tests
- Mobile tests
- UI tests
- Regression tests

Create fixtures for:

- Small files
- Large files
- Corrupted files
- Malformed files
- Password-protected files
- Unicode
- Multiple languages
- Large PDFs
- Complex DOCX
- Complex XLSX
- Images
- Scanned documents

Never assume an input file is valid.

---

# 29. FUZZ TESTING

Fuzz test:

- PDF parser
- DOCX parser
- ZIP parser
- Image parser
- Metadata parser
- OCR input
- Conversion engine

Treat malformed files as security-sensitive inputs.

---

# 30. PLUGIN ARCHITECTURE

Create an extensible plugin system.

Possible plugins:

- New file formats
- AI providers
- Cloud providers
- Storage providers
- Export formats
- Automation tools
- Integrations

Plugin interface must be versioned.

---

# 31. API DESIGN

Build stable APIs.

Example:

```text
POST /documents
GET /documents/{id}
PATCH /documents/{id}
DELETE /documents/{id}

POST /files/upload
GET /files/{id}
POST /files/convert

POST /pdf/merge
POST /pdf/split
POST /pdf/compress

POST /scanner/process
POST /ocr/process

POST /ai/chat
POST /ai/summarize
POST /ai/extract

POST /search
POST /sync
```

Use API versioning.

---

# 32. DATABASE

Start with SQLite for local-first storage where appropriate.

Design cloud synchronization around a server database.

Separate:

- Identity
- Workspace
- Documents
- Blocks
- Files
- File metadata
- Database records
- Tasks
- Comments
- Permissions
- Versions
- Sync changes
- AI metadata
- Embeddings

Do not tightly couple business logic to one database engine.

---

# 33. ERROR HANDLING

Never silently ignore errors.

Every subsystem must have:

- Error codes
- Human-readable errors
- Machine-readable errors
- Logging
- Recovery strategy

Example:

`FILE_CONVERSION_FAILED`

instead of:

`Something went wrong.`

---

# 34. OBSERVABILITY

Implement:

- Structured logs
- Crash reporting
- Performance metrics
- Conversion metrics
- AI latency
- Search latency
- Sync failures

Do not collect sensitive document contents in telemetry.

---

# 35. LICENSING

Before adding an open-source dependency, analyze:

- License
- Commercial usage
- Redistribution
- Attribution
- Linking requirements
- Patent clauses

Maintain:

```text
/THIRD_PARTY_NOTICES
```

Never introduce an incompatible dependency without explicitly flagging it.

---

# 36. DEVELOPMENT PHASES

```text
PHASE 0   Architecture and technology evaluation
PHASE 1   C/C++ core
PHASE 2   SQLite/local storage
PHASE 3   Document/block engine
PHASE 4   File detection and universal file abstraction
PHASE 5   PDF engine
PHASE 6   Scanner
PHASE 7   OCR
PHASE 8   Conversion engine
PHASE 9   Search
PHASE 10  Android
PHASE 11  iOS
PHASE 12  Cloud backend
PHASE 13  Synchronization
PHASE 14  AI
PHASE 15  RAG
PHASE 16  Agent framework
PHASE 17  Collaboration
PHASE 18  Security hardening
PHASE 19  Performance optimization
PHASE 20  Production release
```

---

# 37. AGENT WORKING RULES

Before modifying code:

1. Inspect existing architecture.
2. Understand dependencies.
3. Identify affected modules.
4. Define the intended change.
5. Make the smallest coherent change.
6. Compile.
7. Test.
8. Fix failures.
9. Run regression tests.
10. Update documentation.

Never:

- Rewrite working modules without reason
- Delete tests to make builds pass
- Suppress warnings without justification
- Disable security checks to make functionality work

---

# 38. CODE QUALITY

Follow:

- Clear module boundaries
- Explicit ownership
- Defensive programming
- Thread safety
- Memory safety
- Error propagation
- Secure coding practices

For C code:

- Avoid unsafe string operations
- Validate buffer sizes
- Check integer overflow
- Check allocation failures
- Avoid use-after-free
- Avoid double-free
- Avoid race conditions
- Validate external input

Run:

- Compiler warnings
- Static analyzers
- Sanitizers
- Memory checks
- Fuzz tests

---

# 39. AGENTIC DEVELOPMENT LOOP

For every feature:

```text
REQUIREMENT
    ↓
RESEARCH
    ↓
ARCHITECTURE
    ↓
DEPENDENCY ANALYSIS
    ↓
IMPLEMENTATION
    ↓
UNIT TEST
    ↓
INTEGRATION TEST
    ↓
SECURITY REVIEW
    ↓
PERFORMANCE REVIEW
    ↓
DOCUMENTATION
    ↓
CODE REVIEW
    ↓
COMPLETION
```

Do not move forward if a critical test fails.

---

# 40. FIRST TASK

Do NOT start by implementing features.

First perform a complete architecture and feasibility study.

Produce:

1. Product Requirements Document
2. System Architecture
3. Module Architecture
4. Technology Selection Matrix
5. C vs C++ decision
6. Android architecture
7. iOS architecture
8. Database architecture
9. File-engine architecture
10. PDF architecture
11. OCR architecture
12. AI architecture
13. RAG architecture
14. Agent architecture
15. Offline architecture
16. Sync architecture
17. Security architecture
18. Dependency/license analysis
19. File-format support matrix
20. Conversion matrix
21. Testing strategy
22. Performance strategy
23. Development roadmap
24. Risk register
25. Estimated engineering complexity

Do not write production code until this analysis is complete.

After architecture approval, create the repository structure and implement the smallest working vertical slice:

```text
CREATE NOTE
    ↓
STORE LOCALLY
    ↓
SEARCH NOTE
    ↓
OPEN NOTE
    ↓
EDIT NOTE
    ↓
SAVE NOTE
```

Then add functionality incrementally.

---

# 41. DEFINITION OF DONE

A feature is complete only when:

```text
[ ] Implemented
[ ] Compiles
[ ] Unit tested
[ ] Integration tested where applicable
[ ] Error handling implemented
[ ] Security reviewed
[ ] Performance considered
[ ] Documentation updated
[ ] No known critical defects
[ ] Regression tests pass
```

---

# 42. MOST IMPORTANT RULE

Build this as a real production software platform.

Do not create a fake prototype that merely looks like the product.

Prefer:

**WORKING CORE ENGINE**
over
**SUPERFICIAL UI**

Prefer:

**TESTED FUNCTIONALITY**
over
**FEATURE COUNT**

Prefer:

**SECURITY + DATA INTEGRITY**
over
**SPEED OF IMPLEMENTATION**

Prefer:

**MODULAR ARCHITECTURE**
over
**MONOLITHIC CODE**

Prefer:

**OPEN STANDARDS**
over
**VENDOR LOCK-IN**

Prefer:

**USER PRIVACY**
over
**UNNECESSARY DATA COLLECTION**

---

# 43. FINAL PRODUCT GOAL

The final platform should provide one unified environment for:

```text
NOTES
+
DOCUMENTS
+
DATABASES
+
TASKS
+
CALENDAR
+
PDF
+
SCANNER
+
OCR
+
FILE VIEWER
+
FILE CONVERTER
+
CLOUD STORAGE
+
SEARCH
+
AI
+
RAG
+
AGENTS
+
AUTOMATION
+
COLLABORATION
+
OFFLINE MODE
```

The architecture must allow new file formats, AI models, cloud providers, integrations and features to be added without rewriting the core architecture.

Start with the architecture/feasibility phase.

Do not skip requirements analysis.

Do not pretend unsupported functionality works.

Do not proceed to large-scale implementation until architecture and dependency/license analysis are complete.
