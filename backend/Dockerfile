# Xerces backend image. Railway builds it from the backend/ folder (Root Directory = backend).
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PIP_ROOT_USER_ACTION=ignore

WORKDIR /app

RUN groupadd --system --gid 10001 xerces \
    && useradd --system --uid 10001 --gid xerces --no-create-home --shell /usr/sbin/nologin xerces

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY alembic.ini start.sh ./
COPY app ./app
COPY migrations ./migrations
# Strip CR in case the script was checked out with Windows line endings.
RUN sed -i 's/\r$//' start.sh && chmod 0755 start.sh

# The code stays owned by root; the service runs unprivileged and never writes to disk.
USER xerces

EXPOSE 8080
CMD ["/app/start.sh"]
